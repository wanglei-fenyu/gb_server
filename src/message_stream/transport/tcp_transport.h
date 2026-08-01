#pragma once

#include "itransport.h"

namespace gb
{

/**
 * TCP 传输实现:对应原 ByteStream 的 Socket 逻辑(纯搬运,无逻辑优化)
 */
class TcpTransport : public ITransport
{
public:
    TcpTransport(ByteStream* host, IoService& ios);
    virtual ~TcpTransport();

    virtual void async_connect(const Endpoint& remote_endpoint) override;
    virtual void close() override;
    virtual bool set_connected_options(Endpoint& local_endpoint, bool no_delay) override;
    virtual void async_read_some(char* data, size_t size) override;
    virtual void async_write_some(const char* data, size_t size) override;
    virtual Socket& socket() override;
    virtual SSLSocket* ssl_socket() override;
    virtual bool is_ssl() const override;
    virtual void update_remote_endpoint(Endpoint& remote_endpoint) override;

private:
    void on_connect(const Error_code& error);

private:
    Socket _socket;
};

}
