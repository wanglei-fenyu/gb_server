#include "kcp_listener.h"
#include "base/endpoint_help.h"
#include "ikcp.h"

namespace gb
{

static constexpr size_t kKcpListenerRecvBufferSize = 64 * 1024;

KcpListener::KcpListener(IoService& io, IoServicePoolPtr& io_service_pool, const Endpoint& endpoint)
    : _ios(io)
    , _io_service_pool(io_service_pool)
    , _endpoint(endpoint)
    , _socket(std::make_shared<Asio::ip::udp::socket>(io))
    , _is_closed(true)
{
}

KcpListener::KcpListener(IoServicePoolPtr& io_service_pool, const Endpoint& endpoint)
    : _ios(io_service_pool->GetIoService().second)
    , _io_service_pool(io_service_pool)
    , _endpoint(endpoint)
    , _socket(std::make_shared<Asio::ip::udp::socket>(_ios))
    , _is_closed(true)
{
}

KcpListener::~KcpListener()
{
    close();
}

void KcpListener::close()
{
    std::lock_guard<std::mutex> _lock(_close_mutex);
    if (_is_closed.load())
        return;
    _is_closed.store(true);

    Error_code ec;
    _socket->cancel(ec);
    _socket->close(ec);
    _kcp_map.clear();

    NETWORK_LOG("kcp listener closed: {}", EndpointToString(_endpoint));
}

bool KcpListener::is_close()
{
    std::lock_guard<std::mutex> _lock(_close_mutex);
    return _is_closed.load();
}

void KcpListener::set_create_callback(const callback_t& create_callback)
{
    _create_callback = create_callback;
}

void KcpListener::set_accept_callback(const callback_t& accept_callback)
{
    _accept_callback = accept_callback;
}

void KcpListener::set_accept_fail_callback(const fail_callback_t& accept_fail_callback)
{
    _accept_fail_callback = accept_fail_callback;
}

bool KcpListener::start_listen()
{
    Error_code ec;
    _socket->open(Asio::ip::udp::v4(), ec);
    if (ec)
    {
        NETWORK_LOG("kcp start_listen(): open failed: {}: {}", EndpointToString(_endpoint), ec.message());
        return false;
    }

    _socket->set_option(Asio::ip::udp::socket::reuse_address(true), ec);
    _socket->bind(Asio::ip::udp::endpoint(_endpoint.address(), _endpoint.port()), ec);
    if (ec)
    {
        NETWORK_LOG("kcp start_listen(): bind failed: {}: {}", EndpointToString(_endpoint), ec.message());
        return false;
    }
    _socket->non_blocking(true);

    _is_closed.store(false);
    StartReceive();

    NETWORK_LOG("kcp start_listen(): listen succeed: {}", EndpointToString(_endpoint));
    return true;
}

void KcpListener::StartReceive()
{
    if (_is_closed.load() || !_socket)
        return;

    auto buf = std::make_shared<std::vector<char>>(kKcpListenerRecvBufferSize);
    auto self = shared_from_this();
    _socket->async_receive_from(
        Asio::buffer(buf->data(), buf->size()),
        _sender_endpoint,
        [self, buf](const Error_code& ec, std::size_t bytes_transferred) {
            self->OnReceive(ec, bytes_transferred, buf);
        });
}

void KcpListener::OnReceive(const Error_code& ec,
                            std::size_t bytes_transferred,
                            const std::shared_ptr<std::vector<char>>& buf)
{
    if (_is_closed.load())
        return;

    if (!ec && bytes_transferred > 0)
    {
        uint32_t conv = ikcp_getconv(buf->data());
        auto it = _kcp_map.find(conv);
        if (it != _kcp_map.end())
        {
            SessionPtr session = it->second;
            if (session && !session->is_closed())
            {
                auto data = buf;
                auto n = bytes_transferred;
                Asio::post(session->ioservice(), [session, data, n]() {
                    session->HandleKcpInput(data->data(), static_cast<int>(n));
                });
            }
            else
            {
                // 连接已关闭:清除表项并按新连接处理
                _kcp_map.erase(it);
            }
        }
        else
        {
            auto [io_index, ios] = _io_service_pool->GetIoService();
            SessionPtr session = std::make_shared<Session>(NET_TYPE::NT_SERVER, ios, Endpoint(), TRANSPORT_TYPE::KCP);
            _kcp_map[conv] = session;

            auto self = shared_from_this();
            auto sender = _sender_endpoint;
            auto data = buf;
            auto n = bytes_transferred;
            Asio::post(ios, [self, session, sender, conv, data, n]() {
                if (self->_create_callback)
                    self->_create_callback(session);
                if (self->_accept_callback)
                    self->_accept_callback(session);
                auto shared_socket = self->_socket;
                if (!session->SetupKcp(shared_socket, sender, conv))
                {
                    session->close("kcp setup failed");
                    return;
                }
                session->HandleKcpInput(data->data(), static_cast<int>(n));
            });
        }
    }

    StartReceive();
}

}
