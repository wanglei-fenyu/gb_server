#pragma once

#include "itransport.h"

namespace gb
{

/**
 * SSL/TLS 传输实现:对应原 SSLByteStream 逻辑(纯搬运,无逻辑优化)
 */
class SslTransport : public ITransport
{
public:
    SslTransport(ByteStream* host, NET_TYPE net_type, IoService& ios);
    virtual ~SslTransport();

    virtual void async_connect(const Endpoint& remote_endpoint) override;
    virtual void close() override;
    virtual bool set_connected_options(Endpoint& local_endpoint, bool no_delay) override;
    virtual void async_read_some(char* data, size_t size) override;
    virtual void async_write_some(const char* data, size_t size) override;
    virtual Socket& socket() override;
    virtual SSLSocket* ssl_socket() override;
    virtual bool is_ssl() const override;
    virtual void update_remote_endpoint(Endpoint& remote_endpoint) override;

    virtual void set_ssl_server_file_path(std::string& path, std::string& key_path) override;
    virtual void set_ssl_client_file_path(std::string& path) override;

private:
    void on_connect(const Error_code& error);
    void on_handshake(const Error_code& error);

private:
    IoService& _io_service;
    NET_TYPE   _net_type;
    SSLContext _ssl_context;
    SSLSocket  _socket;
};

}
