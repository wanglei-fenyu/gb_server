#pragma once

#include <cstdint>
#include <chrono>
#include <memory>
#include <vector>
#include "ikcp.h"
#include "message_stream/transport/itransport.h"

namespace gb
{

// KCP 默认 conv(阶段 3 握手后由对端协商覆盖)
constexpr uint32_t kKcpDefaultConv = 0x4B435031; // "KCP1"

// 客户端连接时生成唯一 conv(服务端按 conv 区分多连接,碰撞概率 ~1/2^32)
inline uint32_t GenerateKcpConv()
{
    static std::atomic<uint32_t> seq{0};
    uint32_t t = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    uint32_t s = seq.fetch_add(1) + 1;
    uint32_t conv = t ^ (s << 16) ^ (s >> 16);
    return conv ? conv : 1;
}

/**
 * KCP(可靠 UDP)传输实现
 *
 * 数据通路:
 *   - 发送:async_write_some 追加到 _out_buffer → FlushOutput 按 mss 分段 ikcp_send;
 *           发送窗口满(ikcp_send < 0)时 hold,由 10ms update timer 推进;
 *           回调 _host->on_write_some 的字节数恒 > 0(MessageStream 不接受 0 字节回调,避免死循环)
 *   - 接收:常驻 async_receive_from → ikcp_input → ikcp_recv 排入 _in_buffer;
 *           async_read_some 时立即交付,或挂起 _pending_read 等待数据到达
 *   - 时钟:10ms steady_timer 驱动 ikcp_update(重传/超时/窗口推进)
 *   - 底层:共享 udp::socket(引用计数,阶段 3 服务端多连接复用同一端口),非阻塞 send_to
 *   - 加密:不加密(KCP 不承载 TLS),公网边界走既有 SslTransport;消息级 AEAD 
 *
 * 线程模型:所有操作(connect/read/write/timer/receive 回调)均在 _io_service 线程执行,
 * async_connect 通过 post 切到 io 线程,无锁。
 */
class KcpTransport : public ITransport
{
public:
    KcpTransport(ByteStream* host, IoService& ios, uint32_t conv);
    ~KcpTransport() override;

    void async_connect(const Endpoint& remote_endpoint) override;
    void close() override;
    bool set_connected_options(Endpoint& local_endpoint, bool no_delay) override;
    void async_read_some(char* data, size_t size) override;
    void async_write_some(const char* data, size_t size) override;
    Socket& socket() override;
    SSLSocket* ssl_socket() override;
    bool is_ssl() const override;
    void update_remote_endpoint(Endpoint& remote_endpoint) override;
    bool bind_local(const Endpoint& local_endpoint) override;

    // 服务端共享 socket 模式(KcpListener 分发):绑定共享 udp socket + 对端地址 + conv,
    // 之后由 KcpListener 调用 HandleInput 投递数据,不再自持接收循环
    bool Setup(const std::shared_ptr<Asio::ip::udp::socket>& shared_socket,
               const Asio::ip::udp::endpoint& remote,
               uint32_t conv);
    // 共享 socket 收包入口(必须在 transport 所属 io_context 线程调用,由 KcpListener post 过来)
    void HandleInput(const char* buf, int len);
    // 供 KcpListener 复用共享 socket 与目标 io_context
    std::shared_ptr<Asio::ip::udp::socket>& kcp_socket() { return _socket; }
    IoService& owner_io_service() { return _io_service; }

private:
    static int OutputCallback(const char* buf, int len, ikcpcb* kcp, void* user);
    int SendUdp(const char* buf, int len);

    void DoConnect(const Endpoint& remote_endpoint);

    void StartReceive();
    void OnReceive(const Error_code& ec, std::size_t bytes_transferred, const std::shared_ptr<std::vector<char>>& buf);
    void DrainInput();
    void TryDeliverRead();

    void StartUpdateTimer();
    void OnUpdate(const Error_code& ec);
    void FlushOutput();

    IoService& _io_service;
    std::shared_ptr<Asio::ip::udp::socket> _socket;
    Asio::ip::udp::endpoint _remote_udp_endpoint;
    uint32_t _conv;
    ikcpcb* _kcp;
    Asio::steady_timer _update_timer;
    bool _closed;

    // 接收:已由 KCP 排队、尚未被 async_read_some 取走的应用数据
    std::vector<char> _in_buffer;
    size_t _in_head;
    char* _pending_read;
    size_t _pending_read_size;

    // 发送:已提交(应用层认为已发出)但尚未送入 KCP 的应用数据;窗口满时 hold
    std::vector<char> _out_buffer;
    size_t _out_head;
    bool _flushing;

    // 占位 tcp socket:仅满足接口一致性(KCP 不参与 async_accept)
    Asio::ip::tcp::socket _placeholder_socket;
};

}
