#include "kcp_transport.h"
#include <cstring>
#include "message_stream/byte_stream.h"

namespace gb
{

// 单次 ikcp_send 上限:mss(默认 mtu 1400 - KCP 头 24 = 1376)内单段原子入队;
// ikcp_send 返回 -1 时该段零入队,窗口满不会部分写入
static constexpr size_t kKcpMaxSegment = 1376;

// 接收缓冲(远大于单条 KCP 段)
static constexpr size_t kKcpRecvBufferSize = 64 * 1024;

// update 间隔(ikcp_nodelay interval=10 配合使用)
static constexpr uint32_t kKcpUpdateIntervalMs = 10;

KcpTransport::KcpTransport(ByteStream* host, IoService& ios, uint32_t conv)
    : _io_service(ios)
    , _conv(conv)
    , _kcp(nullptr)
    , _update_timer(ios)
    , _closed(false)
    , _in_head(0)
    , _pending_read(nullptr)
    , _pending_read_size(0)
    , _out_head(0)
    , _flushing(false)
    , _placeholder_socket(ios)
{
    _host = host;
    RESOURCE_COUNTER_INC(KcpTransport);
}

KcpTransport::~KcpTransport()
{
    close();
    RESOURCE_COUNTER_DEC(KcpTransport);
}

void KcpTransport::async_connect(const Endpoint& remote_endpoint)
{
    auto host = _host->shared_from_this();
    Asio::post(_io_service, [host, this, remote_endpoint]() {
        DoConnect(remote_endpoint);
    });
}

void KcpTransport::DoConnect(const Endpoint& remote_endpoint)
{
    if (_closed)
        return;

    _remote_udp_endpoint = Asio::ip::udp::endpoint(remote_endpoint.address(), remote_endpoint.port());

    if (!_socket)
    {
        _socket = std::make_shared<Asio::ip::udp::socket>(_io_service);
        Error_code ec;
        _socket->open(Asio::ip::udp::v4(), ec);
        if (ec)
        {
            _host->close("kcp open udp socket failed: " + ec.message());
            return;
        }
        _socket->set_option(Asio::ip::udp::socket::reuse_address(true), ec);
        _socket->non_blocking(true);
    }
    _kcp = ikcp_create(_conv, this);
    if (!_kcp)
    {
        _host->close("ikcp_create failed");
        return;
    }
    ikcp_setoutput(_kcp, &KcpTransport::OutputCallback);
    ikcp_nodelay(_kcp, 1, kKcpUpdateIntervalMs, 2, 1);
    ikcp_wndsize(_kcp, 128, 128);

    StartReceive();
    StartUpdateTimer();

    // UDP 无连接语义:数据通路就绪即视为已连接(对端可达性由上层握手/超时保证)
    _host->set_socket_connected();
}

void KcpTransport::close()
{
    if (_closed)
        return;
    _closed = true;

    _update_timer.cancel();

    if (_kcp)
    {
        ikcp_release(_kcp);
        _kcp = nullptr;
    }
    // 共享 udp socket 不关闭(其他 KCP 连接可能复用),由引用计数释放
}

bool KcpTransport::bind_local(const Endpoint& local_endpoint)
{
    if (_closed)
        return false;

    if (!_socket)
    {
        _socket = std::make_shared<Asio::ip::udp::socket>(_io_service);
    }
    Error_code ec;
    if (!_socket->is_open())
    {
        _socket->open(Asio::ip::udp::v4(), ec);
        if (ec)
            return false;
    }
    _socket->bind(Asio::ip::udp::endpoint(local_endpoint.address(), local_endpoint.port()), ec);
    if (ec)
        return false;
    _socket->non_blocking(true);
    return true;
}

bool KcpTransport::set_connected_options(Endpoint& local_endpoint, bool no_delay)
{
    if (_socket)
    {
        Error_code ec;
        auto local = _socket->local_endpoint(ec);
        if (!ec)
        {
            local_endpoint = Endpoint(local.address(), local.port());
        }
    }
    return true;
}

void KcpTransport::async_read_some(char* data, size_t size)
{
    if (_in_head < _in_buffer.size())
    {
        size_t available = _in_buffer.size() - _in_head;
        size_t n = std::min(available, size);
        std::memcpy(data, _in_buffer.data() + _in_head, n);
        _in_head += n;
        if (_in_head == _in_buffer.size())
        {
            _in_buffer.clear();
            _in_head = 0;
        }
        _host->on_read_some(Error_code(), n);
        return;
    }

    _pending_read = data;
    _pending_read_size = size;
}

void KcpTransport::async_write_some(const char* data, size_t size)
{
    _out_buffer.insert(_out_buffer.end(), data, data + size);
    FlushOutput();
}

void KcpTransport::FlushOutput()
{
    if (_flushing || _closed || !_kcp)
        return;
    _flushing = true;

    size_t sent_total = 0;
    while (_out_head < _out_buffer.size())
    {
        size_t remaining = _out_buffer.size() - _out_head;
        size_t chunk = std::min(remaining, kKcpMaxSegment);
        int ret = ikcp_send(_kcp, _out_buffer.data() + _out_head, static_cast<int>(chunk));
        if (ret < 0)
        {
            // 发送窗口满:单段零入队,hold 剩余数据,由 update timer 下次推进
            break;
        }
        _out_head += static_cast<size_t>(ret);
        if (_out_head == _out_buffer.size())
        {
            _out_buffer.clear();
            _out_head = 0;
        }
        sent_total += static_cast<size_t>(ret);
    }

    _flushing = false;

    // 窗口满时 hold,不回调 0 字节(MessageStream 收到 0 会死循环)
    if (sent_total > 0)
    {
        _host->on_write_some(Error_code(), sent_total);
    }
}

void KcpTransport::StartReceive()
{
    if (_closed || !_socket)
        return;

    auto buf = std::make_shared<std::vector<char>>(kKcpRecvBufferSize);
    auto host = _host->shared_from_this();
    _socket->async_receive_from(
        Asio::buffer(buf->data(), buf->size()),
        _remote_udp_endpoint,
        [host, this, buf](const Error_code& ec, std::size_t bytes_transferred) {
            OnReceive(ec, bytes_transferred, buf);
        });
}

void KcpTransport::OnReceive(const Error_code& ec, std::size_t bytes_transferred, const std::shared_ptr<std::vector<char>>& buf)
{
    if (_closed || !_kcp)
        return;

    if (ec)
    {
        if (ec != Asio::error::operation_aborted)
        {
            NETWORK_LOG("kcp recv error: {}", ec.message());
        }
        StartReceive();
        return;
    }

    int ret = ikcp_input(_kcp, buf->data(), static_cast<long>(bytes_transferred));
    if (ret < 0)
    {
        NETWORK_LOG("ikcp_input failed: {}", ret);
    }

    DrainInput();
    StartReceive();
}

void KcpTransport::DrainInput()
{
    int peek = ikcp_peeksize(_kcp);
    while (peek > 0)
    {
        std::vector<char> tmp(static_cast<size_t>(peek));
        int n = ikcp_recv(_kcp, tmp.data(), peek);
        if (n <= 0)
        {
            break;
        }
        _in_buffer.insert(_in_buffer.end(), tmp.begin(), tmp.begin() + n);
        peek = ikcp_peeksize(_kcp);
    }

    TryDeliverRead();
}

void KcpTransport::TryDeliverRead()
{
    if (_pending_read && _in_head < _in_buffer.size())
    {
        size_t available = _in_buffer.size() - _in_head;
        size_t n = std::min(available, _pending_read_size);
        std::memcpy(_pending_read, _in_buffer.data() + _in_head, n);
        _pending_read = nullptr;
        _pending_read_size = 0;
        _in_head += n;
        if (_in_head == _in_buffer.size())
        {
            _in_buffer.clear();
            _in_head = 0;
        }
        _host->on_read_some(Error_code(), n);
    }
}

void KcpTransport::StartUpdateTimer()
{
    _update_timer.expires_after(std::chrono::milliseconds(kKcpUpdateIntervalMs));
    auto host = _host->shared_from_this();
    _update_timer.async_wait([host, this](const Error_code& ec) {
        OnUpdate(ec);
    });
}

void KcpTransport::OnUpdate(const Error_code& ec)
{
    if (_closed || ec)
        return;

    if (_kcp)
    {
        uint32_t now_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        ikcp_update(_kcp, now_ms);
    }

    if (_out_head < _out_buffer.size())
    {
        FlushOutput();
    }

    StartUpdateTimer();
}

int KcpTransport::OutputCallback(const char* buf, int len, ikcpcb* kcp, void* user)
{
    return static_cast<KcpTransport*>(user)->SendUdp(buf, len);
}

int KcpTransport::SendUdp(const char* buf, int len)
{
    if (_closed || !_socket)
        return -1;

    Error_code ec;
    _socket->send_to(Asio::buffer(buf, len), _remote_udp_endpoint, 0, ec);
    if (ec && ec != Asio::error::would_block)
    {
        NETWORK_LOG("kcp send_to failed: {}", ec.message());
    }
    // 非阻塞 socket 发送缓冲满(would_block)时丢包,由 KCP 重传兜底
    return len;
}

Socket& KcpTransport::socket()
{
    return _placeholder_socket;
}

SSLSocket* KcpTransport::ssl_socket()
{
    return nullptr;
}

bool KcpTransport::is_ssl() const
{
    return false;
}

void KcpTransport::update_remote_endpoint(Endpoint& remote_endpoint)
{
    remote_endpoint = Endpoint(_remote_udp_endpoint.address(), _remote_udp_endpoint.port());
}

bool KcpTransport::Setup(const std::shared_ptr<Asio::ip::udp::socket>& shared_socket,
                         const Asio::ip::udp::endpoint& remote,
                         uint32_t conv)
{
    if (_closed)
        return false;

    _socket = shared_socket;
    _remote_udp_endpoint = remote;
    _conv = conv;
    _kcp = ikcp_create(_conv, this);
    if (!_kcp)
        return false;
    ikcp_setoutput(_kcp, &KcpTransport::OutputCallback);
    ikcp_nodelay(_kcp, 1, kKcpUpdateIntervalMs, 2, 1);
    ikcp_wndsize(_kcp, 128, 128);

    StartUpdateTimer();
    _host->set_socket_connected();
    return true;
}

void KcpTransport::HandleInput(const char* buf, int len)
{
    if (_closed || !_kcp)
        return;

    int ret = ikcp_input(_kcp, buf, len);
    if (ret < 0)
    {
        NETWORK_LOG("ikcp_input failed: {}", ret);
    }
    DrainInput();
}

}
