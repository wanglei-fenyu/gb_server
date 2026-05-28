#include "network_manager.h"
#include "app/app_def.h"
#include "../net/server.h"

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

uint64_t NetworkManager::GetSequence()
{
    return sequence_tail_.fetch_add(1, std::memory_order_relaxed);
}

void NetworkManager::UnListen(uint32_t type, std::string signal, int level)
{
	//uint64_t key = (((uint64_t)type) << 32) + id;
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
	meta.id = id;
	meta.type = type;
	session->Send(&meta, &msg);
}

void NetworkManager::Send(std::shared_ptr<Session> session, uint32 type, uint64_t id, google::protobuf::Message& msg)
{
	gb::Meta meta;
	meta.id = id;
	meta.type = type;
	session->Send(&meta, &msg);
}

void NetworkManager::ListenOption(uint32_t type, net_listen_fun f, std::string protoName)
{
    uint64_t key              = type;
    std::lock_guard<std::mutex> lock(listen_mutex_);
	listen_function_map_[key] = f;
}

void NetworkManager::CallImpl(RpcCallPtr call, std::string method, sol::variadic_args& args)
{
	if (!call)
	{
		return;
	}
	gb::Meta meta;
	uint64_t    method_key = MD5::MD5Hash64(method.c_str());
	meta.method = method_key;
	uint64_t seq_id = GetSequence();
	meta.sequence = seq_id;
	meta.mode = MsgMode::Request;
	call->SetId(seq_id);
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
    call->BindCurrentExecutor();
    {
        std::lock_guard<std::mutex> lock(rpc_caller_mutex_);
        if (!rpc_caller_map_.insert({meta.sequence, call}).second)
        {
            LOG_ERROR("insert gs_RpcCallerMap fail seq:{} method{}", meta.sequence, meta.method);
        }
    }
	if (buffer && buffer->TotalCount() > 0)
	{
		call->Call(meta, buffer);
	}
	else
	{
		call->Call(meta);
	}
}

void NetworkManager::RpcCancel(int64_t seq_id)
{
    std::lock_guard<std::mutex> lock(rpc_caller_mutex_);
	auto it = rpc_caller_map_.find(seq_id);
	if (it != rpc_caller_map_.end())
		rpc_caller_map_.erase(it);
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

Executor NetworkManager::CreateExecutorForRoute(uint32_t type, uint64_t route_id) const
{
    return Executor::Worker(router_.GetServiceWorker((MessageType)type, route_id));
}

net_listen_fun NetworkManager::FindListenFunction(uint32_t type)
{
    std::lock_guard<std::mutex> lock(listen_mutex_);
    auto                        fun = listen_function_map_.find(type);
    if (fun != listen_function_map_.end())
        return fun->second;
    return {};
}

rpc_listen_fun NetworkManager::FindRpcFunction(uint64_t method)
{
    std::lock_guard<std::mutex> lock(rpc_interface_mutex_);
    auto                        fun = rpc_interface_map_.find(method);
    if (fun != rpc_interface_map_.end())
        return fun->second;
    return {};
}

void NetworkManager::Dispatch(const SessionPtr& session, const ReadBufferPtr& buffer, gb::Meta& meta, int meta_size, int64_t data_size)
{
    switch (meta.mode)
    {
        case MsgMode::Msg:
        {
            auto          executor    = CreateExecutorForRoute(meta.type, meta.id);
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
            auto          executor = CreateExecutorForRoute(meta.type, meta.id);
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
            {
                RpcCallPtr call;
                {
                    std::lock_guard<std::mutex> lock(rpc_caller_mutex_);
                    auto                        it = rpc_caller_map_.find(meta.sequence);
                    if (it != rpc_caller_map_.end())
                    {
                        call = it->second;
                        rpc_caller_map_.erase(it);
                    }
                }
                if (call)
                    call->Done(session, buffer, meta, meta_size, data_size);
            }
            break;
        }
        default:
            break;
    }
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
