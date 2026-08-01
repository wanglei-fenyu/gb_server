#include "tcp_transport.h"
#include "message_stream/byte_stream.h"

namespace gb
{

TcpTransport::TcpTransport(ByteStream* host, IoService& ios)
    : _socket(ios)
{
    _host = host;
    RESOURCE_COUNTER_INC(TcpTransport);
}

TcpTransport::~TcpTransport()
{
    Error_code ec;
    _socket.close(ec);
    RESOURCE_COUNTER_DEC(TcpTransport);
}

void TcpTransport::async_connect(const Endpoint& remote_endpoint)
{
    auto host = _host->shared_from_this();
    _socket.async_connect(remote_endpoint, [host, this](const Error_code& error) {
        on_connect(error);
    });
}

void TcpTransport::on_connect(const Error_code& error)
{
    if (!_host->is_connecting())
    {
        return;
    }
    if (error)
    {
        NETWORK_LOG("on_connect(): connect error: {} ", error.message());
        _host->close("on_connect(): connect error:" + error.message());
        return;
    }

    _host->set_socket_connected();
}

void TcpTransport::close()
{
    Error_code ec;
    _socket.shutdown(Socket::shutdown_both, ec);
}

bool TcpTransport::set_connected_options(Endpoint& local_endpoint, bool no_delay)
{
    Error_code ec;
    _socket.set_option(Asio::ip::tcp::no_delay(no_delay), ec);
    if (ec)
    {
        NETWORK_LOG("set no_delay option failed: {} ", ec.message().c_str());
        _host->close("set no_delay option failed: " + ec.message());
        return false;
    }

    local_endpoint = _socket.local_endpoint(ec);
    if (ec)
    {
        NETWORK_LOG("get local endpoint failed: {} ", ec.message().c_str());
        _host->close("get local endpoint failed: " + ec.message());
        return false;
    }
    return true;
}

void TcpTransport::async_read_some(char* data, size_t size)
{
    auto host = _host->shared_from_this();
    _socket.async_read_some(Asio::buffer(data, size), [host](const Error_code& error, std::size_t bytes_transferred) {
        host->on_read_some(error, bytes_transferred);
    });
}

void TcpTransport::async_write_some(const char* data, size_t size)
{
    auto host = _host->shared_from_this();
    _socket.async_write_some(Asio::buffer(data, size), [host](const Error_code& error, std::size_t bytes_transferred) {
        host->on_write_some(error, bytes_transferred);
    });
}

Socket& TcpTransport::socket()
{
    return _socket;
}

SSLSocket* TcpTransport::ssl_socket()
{
    return nullptr;
}

bool TcpTransport::is_ssl() const
{
    return false;
}

void TcpTransport::update_remote_endpoint(Endpoint& remote_endpoint)
{
    Error_code ec;
    remote_endpoint = _socket.remote_endpoint(ec);
    if (ec)
    {
        NETWORK_LOG("get remote endpoint failed : {} ", ec.message().c_str());
        _host->close("update remote endpoint failed: " + ec.message());
    }
}

}
