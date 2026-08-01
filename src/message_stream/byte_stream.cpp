#include "byte_stream.h"
#include "buffer/buffer.h"
#include "buffer/tran_buf_pool.h"
#include "base/atomic.h"
#include "base/endpoint_help.h"
#include "message_stream/transport/tcp_transport.h"
#include "message_stream/transport/ssl_transport.h"
namespace gb
{

    

 ByteStream::ByteStream(NET_TYPE net_type,IoService& ios, const Endpoint& endpoint, TRANSPORT_TYPE transport_type)
	 : _io_service(ios)
     , _net_type(net_type)
	 , _remote_endpoint(endpoint)
	 , _ticks(0)
	 , _last_rw_ticks(0)
	 , _no_delay(true)
	 , _read_buffer_base_block_factor(TRAN_BUF_BLOCK_MAX_FACTOR)
	 , _write_buffer_base_block_factor(4)
	 , _timer(ios)
	 , _connect_timeout(std::chrono::duration<int64_t, std::ratio<1>>(-1))
	 , _status(NET_STSTUS::STATUS_INIT)
 {
     RESOURCE_COUNTER_INC(ByteStream);

     if (transport_type == TRANSPORT_TYPE::SSL)
     {
         _transport.reset(new SslTransport(this, net_type, ios));
     }
     else
     {
         _transport.reset(new TcpTransport(this, ios));
     }
 }

 ByteStream::~ByteStream()
{
     RESOURCE_COUNTER_DEC(ByteStream);
}

bool ByteStream::no_delay()
{
    return _no_delay;
}

void ByteStream::set_no_delay(bool no_delay)
{
	_no_delay = no_delay;
}

void ByteStream::set_read_buffer_base_block_factor(size_t factor)
{
    _read_buffer_base_block_factor = factor;
}

size_t ByteStream::read_buffer_base_block_factor()
{
    return _read_buffer_base_block_factor;
}

void ByteStream::set_write_buffer_base_block_factor(size_t factor)
{
    _write_buffer_base_block_factor = factor;
}

size_t ByteStream::write_buffer_base_block_factor()
{
    return _write_buffer_base_block_factor;
}

void ByteStream::close(const std::string& reason)
{
	if (atomic_swap(&_status, NET_STSTUS::STATUS_CLOSED) != NET_STSTUS::STATUS_CLOSED)
	{
        _transport->close();
        on_closed();
        if (_remote_endpoint != Endpoint())
		{
            NETWORK_LOG("connection closed: {}:{}", EndpointToString(_remote_endpoint), reason);
		}
	}

}

void ByteStream::on_connect_timeout(const Error_code& error)
{
    if (_status != NET_STSTUS::STATUS_CONNECTING)
        return;
    if (error == Asio::error::operation_aborted)
    {
        return;
    }
    close("connect timeout");
}

void ByteStream::async_connect()
{
     _last_rw_ticks = _ticks;
    
     _status = NET_STSTUS::STATUS_CONNECTING;
     _transport->async_connect(_remote_endpoint);

     if (_connect_timeout.count() > 0)
     {
         _timer.expires_after(std::chrono::duration_cast<Asio::steady_timer::duration>(_connect_timeout));
         _timer.async_wait(asio_bind(&ByteStream::on_connect_timeout,shared_from_this(),_(1)));
     }

}

void ByteStream::update_remote_endpoint()
{
    _transport->update_remote_endpoint(_remote_endpoint);
}

void ByteStream::set_socket_connected()
{
    _last_rw_ticks = _ticks;
    _timer.cancel();

    if (!_transport->set_connected_options(_local_endpoint, _no_delay))
    {
        return;
    }

    if (!on_connected())
    {
        NETWORK_LOG("call on_connected() failed");
        close("call on_connected() failed");
        return;
    }

    _status = NET_STSTUS::STATUS_CONNECTED;
    trigger_receive();
    trigger_send();

}

Socket& ByteStream::socket()
{
    return _transport->socket();
}

SSLSocket* ByteStream::ssl_socket()
{
    return _transport->ssl_socket();
}

IoService& ByteStream::ioservice()
{
    return _io_service;
}

const Endpoint& ByteStream::local_endpoint() const
{
    return _local_endpoint;
}

const Endpoint& ByteStream::remote_endpoint() const
{
    return _remote_endpoint;
}

bool ByteStream::is_connecting() const
{
    return _status == NET_STSTUS::STATUS_CONNECTING;
}

bool ByteStream::is_connected() const
{
    return _status == NET_STSTUS::STATUS_CONNECTED;
}

bool ByteStream::is_closed() const
{
    return _status == NET_STSTUS::STATUS_CLOSED;
}

void ByteStream::reset_ticks(int64_t ticks, bool update_last_rw_ticks)
{
    _ticks = ticks;
    if (update_last_rw_ticks)
    {
        _last_rw_ticks = ticks;
    }
}

int64_t ByteStream::last_rw_ticks()
{
    return _last_rw_ticks;
}

void ByteStream::set_connect_timeout(int64_t timeout)
{
    auto duration_ms = std::chrono::milliseconds(timeout);
    _connect_timeout = std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration_ms);
}

int64_t ByteStream::connect_timeout()
{
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(_connect_timeout);
    return milliseconds.count();
}


void ByteStream::async_read_some(char* data, size_t size)
{
    _transport->async_read_some(data, size);
}

void ByteStream::async_write_some(const char* data, size_t size)
{
    _transport->async_write_some(data, size);
}

void ByteStream::set_ssl_server_file_path_impl(std::string& path, std::string& key_path)
{
    _transport->set_ssl_server_file_path(path, key_path);
}

void ByteStream::set_ssl_client_file_path_impl(std::string& path)
{
    _transport->set_ssl_client_file_path(path);
}

}