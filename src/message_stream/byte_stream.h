#pragma once 

#include <deque>
#include <memory>
#include <string>
#include "define/define.h"
#include "message_stream/transport/itransport.h"


namespace gb
{

//using namespace Asio;
//using Asio::ip::tcp;

class ByteStream : public std::enable_shared_from_this<ByteStream>
{
public:
    ByteStream(NET_TYPE net_type, IoService& ios, const Endpoint& endpoint, TRANSPORT_TYPE transport_type = TRANSPORT_TYPE::TCP);
    virtual ~ByteStream();

    bool no_delay();
    void   set_no_delay(bool no_delay);
    void set_read_buffer_base_block_factor(size_t factor);
    size_t read_buffer_base_block_factor();
    void set_write_buffer_base_block_factor(size_t factor);
    size_t write_buffer_base_block_factor();

    void close(const std::string& reason);

    void on_connect_timeout(const Error_code& error);
    
    void async_connect();

    void update_remote_endpoint();

    void set_socket_connected();

    Socket&    socket();
    SSLSocket* ssl_socket();
    IoService& ioservice();

    const Endpoint&   local_endpoint() const;
    const Endpoint& remote_endpoint() const;

    bool is_connecting() const;
    bool is_connected() const;
    bool is_closed() const;
    

    void reset_ticks(int64_t ticks, bool update_last_rw_ticks);
    int64_t last_rw_ticks();

    void    set_connect_timeout(int64_t timeout /* 毫秒 */);
    int64_t connect_timeout();

    bool is_ssl_socket() { return _transport->is_ssl(); }

    void set_ssl_server_file_path_impl(std::string& path, std::string& key_path);
    void set_ssl_client_file_path_impl(std::string& path);

public:
    virtual bool trigger_receive() = 0;         // 触发接收，成功启动返回true
    virtual bool trigger_send() = 0;            // 触发发送，成功启动返回true
    
    void async_read_some(char* data, size_t size);           // 异步读取数据
    void async_write_some(const char* data, size_t size);    // 异步写入数据

    virtual bool on_connected() = 0;

    virtual void on_closed() = 0;

    virtual void on_read_some(const Error_code& error,std::size_t bytes_transferred) = 0;

    virtual void on_write_some(const Error_code& error,std::size_t bytes_transferred) = 0;

protected:
    NET_TYPE _net_type;

protected:
    IoService&  _io_service;
    Endpoint                              _local_endpoint;
    Endpoint                              _remote_endpoint;
    int64_t _ticks;
    int64_t _last_rw_ticks;
    bool                                  _no_delay;

    size_t _read_buffer_base_block_factor;
    size_t _write_buffer_base_block_factor;

private:
    Asio::steady_timer _timer;
    std::chrono::steady_clock::duration _connect_timeout;       // 连接超时持续时间
    std::atomic<NET_STSTUS>             _status;
    std::unique_ptr<ITransport>         _transport;
};


}
