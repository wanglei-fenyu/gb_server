#pragma once

#include <cstddef>
#include <string>
#include "define/define.h"

namespace gb
{

class ByteStream;

/**
 * 传输层抽象:TCP / SSL / KCP 统一接口
 *
 * ByteStream(门面)持有 unique_ptr<ITransport>,连接建立、读写均转发给 transport;
 * transport 通过 _host(ByteStream*) 回调门面的公共方法:
 *   - 连接成功     → _host->set_socket_connected()
 *   - 读/写完成    → _host->on_read_some / _host->on_write_some
 *   - 连接/握手失败 → _host->close(reason)
 *
 * 实现类在异步回调中持有 _host->shared_from_this(),保证 IO 完成前 ByteStream 不析构。
 */
class ITransport
{
public:
    virtual ~ITransport() = default;

    // 发起异步连接,成功后由实现调用 _host->set_socket_connected()
    virtual void async_connect(const Endpoint& remote_endpoint) = 0;

    // 关闭底层通道(仅 shutdown,不触发 on_closed;由 ByteStream::close 统一处理)
    virtual void close() = 0;

    // 连接建立后的选项配置(no_delay、local_endpoint 采集),失败返回 false 并已 close
    virtual bool set_connected_options(Endpoint& local_endpoint, bool no_delay) = 0;

    // 异步读写,完成后回调 _host->on_read_some / _host->on_write_some
    virtual void async_read_some(char* data, size_t size) = 0;
    virtual void async_write_some(const char* data, size_t size) = 0;

    // 底层 socket 访问(TCP 直接返回;SSL 返回 next_layer;KCP 返回占位 socket)
    virtual Socket& socket() = 0;

    // SSL socket 访问(仅 SslTransport 返回非空,用于服务端握手)
    virtual SSLSocket* ssl_socket() = 0;

    virtual bool is_ssl() const = 0;

    // 刷新对端地址(供日志/回调使用)
    virtual void update_remote_endpoint(Endpoint& remote_endpoint) = 0;

    // 绑定本地地址(仅 KcpTransport 需要;TCP 走 connect 内核绑定,默认空实现)
    virtual bool bind_local(const Endpoint& local_endpoint)
    {
        (void)local_endpoint;
        return true;
    }

    // 服务端共享 socket 接管(仅 KcpTransport 需要):绑定共享 udp socket + 对端 + conv,
    // 由 KcpListener 转发 HandleInput。TCP/SSL 默认不可用。
    virtual bool Setup(const std::shared_ptr<Asio::ip::udp::socket>& shared_socket,
                       const Asio::ip::udp::endpoint& remote,
                       uint32_t conv)
    {
        (void)shared_socket;
        (void)remote;
        (void)conv;
        return false;
    }
    virtual void HandleInput(const char* buf, int len)
    {
        (void)buf;
        (void)len;
    }

    // SSL 证书路径配置(默认空实现,仅 SslTransport 覆写)
    virtual void set_ssl_server_file_path(std::string& path, std::string& key_path) {}
    virtual void set_ssl_client_file_path(std::string& path) {}

protected:
    ByteStream* _host = nullptr;
};

}
