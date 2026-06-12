#include "network_manager.h"
#include "app/types.h"
#include "network/io/server.h"
#include "worker/worker.h"
#include "worker/worker_manager.h"

NAMESPACE_BEGIN(gb)

void NetworkManager::Init(HandleInterface* handleInterface)
{
	if (!handleInterface)
	{
        LOG_ERROR("handle interface is nullptr");
        return;
	}
	auto [ip, port] = AppTypeMgr::Instance()->GetServerIpPort();
	std::string uir = ip + ":" + port;

    handleInterface->SetReceivedCallBack([this](const SessionPtr& session, const ReadBufferPtr& buffer, int meta_size, int64_t data_size) {
		OnReceiveCall(session, buffer, meta_size, data_size);
	});
	
}

void NetworkManager::UnListen(uint32_t type, std::string signal, int level)
{
	// uint64_t key = (((uint64_t)type) << 32) + id;
    uint32_t key      = type;
    std::lock_guard<std::mutex> lock(listen_mutex_);
	auto p_FunsIt = listen_function_map_.find(key);
	if (listen_function_map_.end() == p_FunsIt)
	{
		LOG_ERROR("message don't listen type:{}", type);
		return;
	}
	{
		listen_function_map_.erase(p_FunsIt);
	}
}

void NetworkManager::Send(Session* session, uint32_t type, uint64_t id, google::protobuf::Message& msg)
{
	gb::Meta meta;
	meta.user_unique_id = id;
	meta.type = type;
	session->Send(&meta, &msg);
}

void NetworkManager::Send(std::shared_ptr<Session> session, uint32 type, uint64_t id, google::protobuf::Message& msg)
{
	gb::Meta meta;
	meta.user_unique_id = id;
	meta.type = type;
	session->Send(&meta, &msg);
}

void NetworkManager::Send(Session* session, const Meta& meta, google::protobuf::Message& msg)
{
	session->Send(&meta, &msg);
}

void NetworkManager::Send(std::shared_ptr<Session> session, const Meta& meta, google::protobuf::Message& msg)
{
	session->Send(&meta, &msg);
}

void NetworkManager::ListenOption(uint32_t type, net_listen_fun f, std::string protoName)
{
    uint64_t key              = type;
    std::lock_guard<std::mutex> lock(listen_mutex_);
	listen_function_map_[key] = f;
}

void NetworkManager::CallImpl(RpcCallPtr call, std::string method, uint64_t id, sol::variadic_args& args)
{
	if (!call)
		return;
	gb::Meta meta;
	uint64_t method_key = MD5::MD5Hash64(method.c_str());
	meta.method = method_key;
	meta.mode = MsgMode::Request;
	meta.user_unique_id = id;
	// sequence和call->SetId在CallImpl(Meta&, RpcCallPtr, ReadBufferPtr)内部处理
	if (args.size() > 0)
	{
		std::vector<uint8_t> data = gb::msgpack::pack(args);
		WriteBuffer          write_buffer;
		write_buffer.Append((const char *)data.data(), data.size());
		ReadBufferPtr read_buffer(new ReadBuffer());
		write_buffer.SwapOut(read_buffer.get());
		CallImpl(meta, call, read_buffer);
	}
	else
	{
		CallImpl(meta, call);
	}
}

void NetworkManager::CallImpl(gb::Meta& meta, RpcCallPtr call, const ReadBufferPtr buffer)
{
	if (!call)
		return;

	// 将(worker_index << 32 | local_seq)编码到64位sequence字段
	// IO线程稍后解码此信息，将响应路由到正确的Worker
	// 无需访问任何共享映射
	auto worker = WorkerManager::Instance()->GetCurWorker();
	if (!worker)
	{
		LOG_ERROR("RPC call must be made from a worker thread");
		return;
	}

	uint32_t local_seq    = worker->AllocRpcSeq();
	uint32_t worker_index = worker->GetIndex();

	SequenceId sid;
	sid.index = worker_index;
	sid.seq   = local_seq;
	meta.sequence = sid.value;
	call->SetId(sid.value);
	call->SetWorkerInfo(worker_index, local_seq);

	// 存储到当前Worker的线程局部待处理映射中——无锁
	// 响应（或超时）将通过Worker::TakePendingRpc(local_seq)移除它
	// 在同一个Worker线程中
	worker->StorePendingRpc(local_seq, call);

	call->BindCurrentExecutor();

	if (buffer && buffer->TotalCount() > 0)
		call->Call(meta, buffer);
	else
		call->Call(meta);
}

void NetworkManager::RegisterOption(std::string method, rpc_listen_fun fn)
{
	uint64_t key = MD5::MD5Hash64(method.c_str());
    std::lock_guard<std::mutex> lock(rpc_interface_mutex_);
	rpc_interface_map_.insert({key, fn});
}

void NetworkManager::UnRegister(std::string method)
{
	uint64_t key = MD5::MD5Hash64(method.c_str());
    std::lock_guard<std::mutex> lock(rpc_interface_mutex_);
	rpc_interface_map_.erase(key);
}

net_listen_fun NetworkManager::FindListenFunction(uint32_t type)
{
    // 快速路径：从冻结的只读映射读取——无锁
    auto* frozen = frozen_listen_map_.load(std::memory_order_acquire);
    if (frozen)
    {
        auto it = frozen->find(type);
        if (it != frozen->end())
            return it->second;
        return {};
    }
    // 回退：从可变映射读取（冻结前）
    std::lock_guard<std::mutex> lock(listen_mutex_);
    auto it = listen_function_map_.find(type);
    if (it != listen_function_map_.end())
        return it->second;
    return {};
}

rpc_listen_fun NetworkManager::FindRpcFunction(uint64_t method)
{
    // 快速路径：从冻结的只读映射读取——无锁
    auto* frozen = frozen_rpc_interface_map_.load(std::memory_order_acquire);
    if (frozen)
    {
        auto it = frozen->find(method);
        if (it != frozen->end())
            return it->second;
        return {};
    }
    // 回退：从可变映射读取（冻结前）
    std::lock_guard<std::mutex> lock(rpc_interface_mutex_);
    auto it = rpc_interface_map_.find(method);
    if (it != rpc_interface_map_.end())
        return it->second;
    return {};
}

void NetworkManager::Dispatch(const SessionPtr& session, const ReadBufferPtr& buffer, gb::Meta& meta, int meta_size, int64_t data_size)
{
    switch (meta.mode)
    {
        case MsgMode::Msg:
        {
            // 统一路由入口：根据 ServerApp::SetRouterPolicy 设定的策略自动走对应路径
            //   Stateful：按 scene_id（优先）或 user_unique_id 精确绑定
            //   Stateless：纯 hash 路由
            auto           executor    = router_.GetExecutor(meta.type, meta.user_unique_id);
            net_listen_fun listen_func = FindListenFunction(meta.type);
            if (!listen_func)
                return;
            executor.Dispatch([session = session, buffer = buffer, meta = meta, meta_size, data_size, listen_func = std::move(listen_func)]() mutable {
                listen_func(session, buffer, meta, meta_size, data_size);
            });
            break;
        }
        case MsgMode::Request:
        {
            auto           executor  = router_.GetExecutor(meta.type, meta.user_unique_id);
            rpc_listen_fun rpc_func = FindRpcFunction(meta.method);
            if (!rpc_func)
                return;
            executor.Dispatch([session = session, buffer = buffer, meta = meta, meta_size, data_size, rpc_func = std::move(rpc_func)]() mutable {
                rpc_func(session, buffer, meta, meta_size, data_size);
            });
            break;
        }
        case MsgMode::Response:
        {
            // 从64位sequence字段解码worker_index + local_seq
            SequenceId sid;
            sid.value       = meta.sequence;
            uint32_t worker_index = sid.index;
            uint32_t local_seq    = sid.seq;

            auto worker_mgr = WorkerManager::Instance();
            auto worker     = worker_mgr->GetWorker(worker_index);
            if (!worker)
            {
                LOG_WARN("Response for unknown worker index: {}", worker_index);
                return;
            }

            worker->Post([session, buffer, meta, meta_size, data_size, local_seq, worker]() mutable {
                auto call = worker->TakePendingRpc(local_seq);
                if (call)
                    call->Done(session, buffer, meta, meta_size, data_size);
            });
            break;
        }
        default:
            break;
    }
}

void NetworkManager::Freeze()
{
    // 对可变映射创建冻结快照
    // 此后，所有读取都通过原子指针进行——无锁
    // 两个映射只分配一次，永不释放（有意为之）
    // Freeze()之后的修改被静默忽略（写入可变映射，但
    // 读取只看到冻结版本）
    auto* listen_snapshot = new ListenMap();
    auto* rpc_snapshot    = new RpcInterfaceMap();
    {
        std::lock_guard<std::mutex> lock(listen_mutex_);
        *listen_snapshot = listen_function_map_;
        listen_function_map_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(rpc_interface_mutex_);
        *rpc_snapshot = rpc_interface_map_;
        rpc_interface_map_.clear();
    }
    frozen_listen_map_.store(listen_snapshot, std::memory_order_release);
    frozen_rpc_interface_map_.store(rpc_snapshot, std::memory_order_release);
    LOG_INFO("NetworkManager frozen: {} listeners, {} RPC methods",
             listen_snapshot->size(), rpc_snapshot->size());
}

NetworkManager::~NetworkManager()
{
    delete frozen_listen_map_.load();
    delete frozen_rpc_interface_map_.load();
}

void NetworkManager::OnReceiveCall(const SessionPtr& session, const ReadBufferPtr& buffer, int meta_size, int64_t data_size)
{
	gb::Meta meta;
    if (!ReadMeta(buffer.get(), meta, meta_size))
    {
        LOG_ERROR("read meta failed!");
        return;
    }
	Dispatch(session, buffer, meta, meta_size, data_size);
}

NAMESPACE_END
