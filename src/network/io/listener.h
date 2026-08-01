#pragma once

#include "network/io/session.h"
#include "network/io/io_service_pool.h"
#include "define/define.h"
namespace gb
{

class Listener : public std::enable_shared_from_this<Listener>
{
    using callback_t = std::function<void(SessionPtr)>;     
    using fail_callback_t = std::function<void(NET_ErrorCode,std::string_view /*原因*/)>;

public:
	static const int LISTEN_MAX_CONNECTIONS = 4096;

public:
    Listener(IoService& io, IoServicePoolPtr& io_service_pool, const Endpoint& endpoint, TRANSPORT_TYPE transport_type = TRANSPORT_TYPE::TCP);
    Listener(IoServicePoolPtr& io_service_pool, const Endpoint& endpoint, TRANSPORT_TYPE transport_type = TRANSPORT_TYPE::TCP);
    virtual ~Listener();

    void close();
    bool is_close();
    void set_create_callback(const callback_t& create_callback);
    void set_accept_callback(const callback_t& accept_callback);
    void set_accept_fail_callback(const fail_callback_t& accept_fail_callback);
    bool start_listen();

private:
    void async_accept();
    void on_accept(const SessionPtr& session, const Error_code& ec);

private:
    IoService& _ios;
    IoServicePoolPtr& _io_service_pool;
    Endpoint   _endpoint;
    Acceptor   _acceptor;
    TRANSPORT_TYPE _transport_type;

    callback_t _create_callback;
    callback_t _accept_callback;
    fail_callback_t _accept_fail_callback;

    std::atomic<bool> _is_closed;
    std::mutex        _close_mutex;

	NON_COPYABLE(Listener);
};

using ListenerPtr = std::shared_ptr<Listener>;
}