#pragma once

#include "network/io/session.h"
#include "network/io/io_service_pool.h"
#include <unordered_map>
namespace gb
{

/**
 * KCP 服务端监听器:单个共享 udp socket 常驻接收循环,按 conv 分发到各 KcpTransport
 *
 * 连接建立(无显式 SYN/ACK):
 *   - 客户端 KcpTransport 用随机 conv 发首个 KCP 包(SYN 段含 conv)
 *   - 本监听器收到未知 conv → 创建新 Session(KcpTransport 复用共享 socket)
 *     → post 到该 Session 的 io_context 完成 Setup + 回调,再投递首包
 *   - conv 即会话 key;连接关闭后残留表项由下次同 conv 收包时懒清理
 *
 * 线程模型:_kcp_map 只在监听器 io_context 线程读写(无锁);
 * Setup/HandleInput 一律 post 到各 Session 自己的 io_context,避免与 update timer 竞争
 */
class KcpListener : public std::enable_shared_from_this<KcpListener>
{
    using callback_t = std::function<void(SessionPtr)>;
    using fail_callback_t = std::function<void(NET_ErrorCode, std::string_view)>;

public:
    KcpListener(IoService& io, IoServicePoolPtr& io_service_pool, const Endpoint& endpoint);
    KcpListener(IoServicePoolPtr& io_service_pool, const Endpoint& endpoint);
    virtual ~KcpListener();

    void close();
    bool is_close();
    void set_create_callback(const callback_t& create_callback);
    void set_accept_callback(const callback_t& accept_callback);
    void set_accept_fail_callback(const fail_callback_t& accept_fail_callback);
    bool start_listen();

private:
    void StartReceive();
    void OnReceive(const Error_code& ec,
                   std::size_t bytes_transferred,
                   const std::shared_ptr<std::vector<char>>& buf);

private:
    IoService& _ios;
    IoServicePoolPtr& _io_service_pool;
    Endpoint _endpoint;
    std::shared_ptr<Asio::ip::udp::socket> _socket;
    Asio::ip::udp::endpoint _sender_endpoint;
    std::unordered_map<uint32_t, SessionPtr> _kcp_map;

    callback_t _create_callback;
    callback_t _accept_callback;
    fail_callback_t _accept_fail_callback;

    std::atomic<bool> _is_closed;
    std::mutex _close_mutex;

    NON_COPYABLE(KcpListener);
};

using KcpListenerPtr = std::shared_ptr<KcpListener>;
}
