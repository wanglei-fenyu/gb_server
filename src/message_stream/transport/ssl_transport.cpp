#include "ssl_transport.h"
#include "message_stream/byte_stream.h"
#include <openssl/ssl.h>

namespace gb
{

SslTransport::SslTransport(ByteStream* host, NET_TYPE net_type, IoService& ios)
    : _io_service(ios)
    , _net_type(net_type)
    , _ssl_context(net_type == NET_TYPE::NT_SERVER ? Asio::ssl::context::tlsv12_server : Asio::ssl::context::tlsv12_client)
    , _socket(ios, _ssl_context)
{
    _host = host;
    RESOURCE_COUNTER_INC(SslTransport);
}

SslTransport::~SslTransport()
{
    Error_code ec;
    _socket.lowest_layer().close(ec);
    RESOURCE_COUNTER_DEC(SslTransport);
}

void SslTransport::async_connect(const Endpoint& remote_endpoint)
{
    auto host = _host->shared_from_this();
    _socket.lowest_layer().async_connect(remote_endpoint, [host, this](const Error_code& error) {
        on_connect(error);
    });
}

// 与原始 SSLByteStream::on_connect 一致:第一层连接错误不检查,直接进入握手
void SslTransport::on_connect(const Error_code& error)
{
    (void)error;
    auto host = _host->shared_from_this();
    _socket.async_handshake(Asio::ssl::stream_base::client, [host, this](const Error_code& hs_error) {
        on_handshake(hs_error);
    });
}

void SslTransport::on_handshake(const Error_code& error)
{
    if (!error)
    {
        if (!_host->is_connecting())
        {
            return;
        }
        _host->set_socket_connected();
    }
    else
    {
        NET_LOG_ERROR("handshake error: {}", error.message());
        _host->close("handshake failed:" + error.message());
    }
}

void SslTransport::close()
{
    Error_code ec;
    _socket.lowest_layer().shutdown(Socket::shutdown_both, ec);
}

bool SslTransport::set_connected_options(Endpoint& local_endpoint, bool no_delay)
{
    Error_code ec;
    _socket.lowest_layer().set_option(Asio::ip::tcp::no_delay(no_delay), ec);
    if (ec)
    {
        NETWORK_LOG("set no_delay option failed: {} ", ec.message().c_str());
        _host->close("set no_delay option failed: " + ec.message());
        return false;
    }

    local_endpoint = _socket.lowest_layer().local_endpoint(ec);
    if (ec)
    {
        NETWORK_LOG("get local endpoint failed: {} ", ec.message().c_str());
        _host->close("get local endpoint failed: " + ec.message());
        return false;
    }
    return true;
}

void SslTransport::async_read_some(char* data, size_t size)
{
    auto host = _host->shared_from_this();
    _socket.async_read_some(Asio::buffer(data, size), [host](const Error_code& error, std::size_t bytes_transferred) {
        host->on_read_some(error, bytes_transferred);
    });
}

void SslTransport::async_write_some(const char* data, size_t size)
{
    auto host = _host->shared_from_this();
    _socket.async_write_some(Asio::buffer(data, size), [host](const Error_code& error, std::size_t bytes_transferred) {
        host->on_write_some(error, bytes_transferred);
    });
}

Socket& SslTransport::socket()
{
    return _socket.next_layer();
}

SSLSocket* SslTransport::ssl_socket()
{
    return &_socket;
}

bool SslTransport::is_ssl() const
{
    return true;
}

void SslTransport::update_remote_endpoint(Endpoint& remote_endpoint)
{
    Error_code ec;
    remote_endpoint = _socket.lowest_layer().remote_endpoint(ec);
    if (ec)
    {
        NETWORK_LOG("get remote endpoint failed : {} ", ec.message().c_str());
        _host->close("update remote endpoint failed: " + ec.message());
    }
}

void SslTransport::set_ssl_server_file_path(std::string& path, std::string& key_path)
{
    try
    {
        _ssl_context.set_options(Asio::ssl::context::default_workarounds |
                                 Asio::ssl::context::no_sslv2 |
                                 Asio::ssl::context::no_sslv3 |
                                 Asio::ssl::context::tlsv12);

        if (_net_type == NET_TYPE::NT_SERVER)
        {
            _ssl_context.use_certificate_chain_file(path);
            _ssl_context.use_private_key_file(key_path, Asio::ssl::context::pem);
        }
        _socket = SSLSocket(_io_service, _ssl_context);
    }
    catch (const std::exception& e)
    {
        NETWORK_LOG("SSL configuration error: {}", e.what());
        throw;
    }
}

void SslTransport::set_ssl_client_file_path(std::string& path)
{
    try
    {
        _ssl_context.set_options(Asio::ssl::context::default_workarounds |
                                 Asio::ssl::context::no_sslv2 |
                                 Asio::ssl::context::no_sslv3 |
                                 Asio::ssl::context::tlsv12);

        if (_net_type == NET_TYPE::NT_CLIENT)
        {
            _ssl_context.load_verify_file(path);
            _ssl_context.set_verify_mode(Asio::ssl::verify_peer);
        }
        _socket = SSLSocket(_io_service, _ssl_context);
    }
    catch (const std::exception& e)
    {
        NETWORK_LOG("SSL configuration error: {}", e.what());
        throw;
    }
}

}
