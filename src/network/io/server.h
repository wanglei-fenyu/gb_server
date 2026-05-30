#pragma once 
#include "listener.h"
#include "gbnet/common/define.h"
#include "network/io/timer_worker.h"
#include "handle_interface.h"
namespace gb
{

struct ServerOptions
{
    //int work_thread_num;        //缃戠粶澶勭悊绾跨▼鏁?
    int max_connection_count;   //鍏佽鐨勬渶澶ц繛鎺ユ暟閲? -1娌℃湁闄愬埗
    int keep_alive_time;        //淇濇寔杩炴帴鐨勬椂闂?-1娌℃湁闄愬埗
    int max_pending_buffer_size;//涓€涓繛鎺ョ瓑寰呭彂閫侀槦鍒楁渶澶х紦鍐插尯 鍗曚綅MB  0琛ㄧず娌℃湁缂撳啿鍖?榛樿100MB

    //缃戠粶鍚炲悙 -1琛ㄧず娌℃湁闄愬埗
    int max_throughput_in;
    int max_throughput_out;
    
    size_t io_service_pool_size; //瀹為檯缃戠粶澶勭悊绾跨▼鏁?

    //涓€涓熀鏈潡64B  榛樿鏄?  鏃堕棿鍐呭瓨澶у皬  64<<4 1024B
    size_t write_buffer_base_block_factor;
    size_t read_buffer_base_block_factor;  
    
    bool no_delay;  //榛樿true  

	ServerOptions()
	//: work_thread_num(8)
	: max_connection_count(-1)
	, keep_alive_time(-1)
	, max_pending_buffer_size(100)
	, max_throughput_in(-1)
	, max_throughput_out(-1)
	, io_service_pool_size(4)
	, write_buffer_base_block_factor(4)
	, read_buffer_base_block_factor(9)
	, no_delay(true)
    {}
};



class ServerImpl :public std::enable_shared_from_this<ServerImpl>
{
public:
    inline static const int MAINTAIN_INTERVAL_IN_MS = 100;     //缁存姢闂撮殧 100ms

public:
    ServerImpl(const ServerOptions& options);
    virtual ~ServerImpl();

    bool Start(std::string_view server_address);
    void Stop();

    time_point_t GetStartTime();

    ServerOptions GetOptions();

    void ResetOptions(const ServerOptions& options);
    
    int ConnectionCount();

    void GetPendingStat(int64_t* pending_message_count, int64_t* pending_buffer_size, int64_t* pending_data_size);

    bool IsListening();

    bool ReStartListen();

    IoServicePoolPtr GetIoServicePool();

public:
    void SetReceivedCallBack(Session::session_received_callback_t callback);
    void SetConnnectCallBack(Session::session_connected_callback_t callback);
    void SetCloseCallBack(Session::session_closed_callback_t callback);

private:
    void OnCreated(const SessionPtr& session);

    void OnAccepted(const SessionPtr& session);

    void OnAcceptedFailed(NET_ErrorCode error_code, const std::string_view reason);

    void OnReceived(const SessionPtr& session, const ReadBufferPtr& buffer, int meta_size,int64_t data_size);

    void OnClosed(const SessionPtr& stream);

    void StopSession();
    void ClearSession();

    void TimerMaintain(const time_point_t& now);


private:
    IoServicePoolPtr _io_service_pool;
    Endpoint      _listen_endpoint;
    ListenerPtr   _listener;
    ServerOptions _options;

    bool _is_runing;
    time_point_t _start_time;
    int64_t _ticks_per_second;
    int64_t _last_maintain_ticks;
    int64_t _last_restart_listen_ticks;
    int64_t _last_switch_stat_slot_ticks;
    int64_t _last_print_connection_ticks;
    int64_t _restart_listen_interval_ticks;
    std::mutex _start_stop_mutex;

    int64_t _slice_count;
    int64_t _slice_quota_in;
    int64_t _slice_quota_out;
    int64_t _max_pending_buffer_size;
    int64_t _keep_alive_ticks;
    
    FlowControllerPtr _flow_controller;

    std::set<SessionPtr> _session_set;
    std::mutex           _session_set_mutex;
    
    TimerWorkerPtr  _timer_worker;
    IoWorkerPtr _maintain_thread;

private:
    Session::session_received_callback_t  _received_callback;
    Session::session_connected_callback_t _connected_callback; 
    Session::session_closed_callback_t    _close_callback;

    NON_COPYABLE(ServerImpl);
    
};


class Server : public HandleInterface
{

public:
    explicit Server(const ServerOptions& option = ServerOptions());
    virtual ~Server();
    
    bool Start(const std::string& server_address);
    void Stop();

    ServerOptions GetOptions();
    void ResetOptions(const ServerOptions& options);

    int ConnectionCount();
    bool IsListening();

    const std::shared_ptr<ServerImpl>& impl(); 
    
    virtual void SetReceivedCallBack(Session::session_received_callback_t callback) override;
    virtual void SetConnnectCallBack(Session::session_connected_callback_t callback) override;
    virtual void SetCloseCallBack(Session::session_closed_callback_t callback) override;
    
    IoServicePoolPtr GetIoServicePool();

private:
    
    std::shared_ptr<ServerImpl> _impl;
};

}