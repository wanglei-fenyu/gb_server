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
		return;
	gb::Meta meta;
	uint64_t method_key = MD5::MD5Hash64(method.c_str());
	meta.method = method_key;
	meta.mode = MsgMode::Request;
	// sequence鍜宑all->SetId鍦–allImpl(Meta&, RpcCallPtr, ReadBufferPtr)鍐呴儴澶勭悊
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

	// 灏?worker_index << 32 | local_seq)缂栫爜鍒?4浣峴equence瀛楁
	// IO绾跨▼绋嶅悗瑙ｇ爜姝や俊鎭紝灏嗗搷搴旇矾鐢卞埌姝ｇ‘鐨刉orker
	// 鏃犻渶璁块棶浠讳綍鍏变韩鏄犲皠
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

	// 瀛樺偍鍒板綋鍓峎orker鐨勭嚎绋嬪眬閮ㄥ緟澶勭悊鏄犲皠涓?鈥?鏃犻攣
	// 鍝嶅簲锛堟垨瓒呮椂锛夊皢閫氳繃Worker::TakePendingRpc(local_seq)绉婚櫎瀹?
	// 鍦ㄥ悓涓€涓猈orker绾跨▼涓?
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

WorkerExecutor NetworkManager::CreateExecutorForRoute(uint32_t type, uint64_t route_id) const
{
    return router_.GetServiceExecutor((MessageType)type, route_id);
}

net_listen_fun NetworkManager::FindListenFunction(uint32_t type)
{
    // 蹇€熻矾寰勶細浠庡喕缁撶殑鍙鏄犲皠璇诲彇 鈥?鏃犻攣
    auto* frozen = frozen_listen_map_.load(std::memory_order_acquire);
    if (frozen)
    {
        auto it = frozen->find(type);
        if (it != frozen->end())
            return it->second;
        return {};
    }
    // 鍥為€€锛氫粠鍙彉鏄犲皠璇诲彇锛堝喕缁撳墠锛?
    std::lock_guard<std::mutex> lock(listen_mutex_);
    auto it = listen_function_map_.find(type);
    if (it != listen_function_map_.end())
        return it->second;
    return {};
}

rpc_listen_fun NetworkManager::FindRpcFunction(uint64_t method)
{
    // 蹇€熻矾寰勶細浠庡喕缁撶殑鍙鏄犲皠璇诲彇 鈥?鏃犻攣
    auto* frozen = frozen_rpc_interface_map_.load(std::memory_order_acquire);
    if (frozen)
    {
        auto it = frozen->find(method);
        if (it != frozen->end())
            return it->second;
        return {};
    }
    // 鍥為€€锛氫粠鍙彉鏄犲皠璇诲彇锛堝喕缁撳墠锛?
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
            auto           executor    = CreateExecutorForRoute(meta.type, meta.id);
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
            auto           executor  = CreateExecutorForRoute(meta.type, meta.id);
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
            // 浠?4浣峴equence瀛楁瑙ｇ爜worker_index + local_seq
            // IO绾跨▼浠庝笉璁块棶鍏变韩鏄犲皠 鈥?鍙В鐮佸拰杞彂
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

            // 鎶曢€掑埌鎵€灞濿orker绾跨▼锛屼互渚垮湪鍏剁嚎绋嬪眬閮ㄥ緟澶勭悊鏄犲皠涓煡鎵捐皟鐢?
            //锛圵orker绔棤閿侊級
            worker->Post([session, buffer, meta, meta_size, data_size, local_seq]() mutable {
                auto call = Worker::TakePendingRpc(local_seq);
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
    // 瀵瑰彲鍙樻槧灏勫垱寤哄喕缁撳揩鐓?
    // 姝ゅ悗锛屾墍鏈夎鍙栭兘閫氳繃鍘熷瓙鎸囬拡杩涜 鈥?鏃犻攣
    // 涓や釜鏄犲皠鍙垎閰嶄竴娆★紝姘镐笉閲婃斁锛堟湁鎰忎负涔嬶級
    // Freeze()涔嬪悗鐨勪慨鏀硅闈欓粯蹇界暐锛堝啓鍏ュ彲鍙樻槧灏勶紝浣?
    // 璇诲彇鍙湅鍒板喕缁撶増鏈級
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
