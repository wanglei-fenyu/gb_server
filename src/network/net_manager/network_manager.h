#pragma once 
#include "gbnet/common/def.h"
#include "gbnet/common/define.h"
#include "network/session/session.h"
#include "network/network_function.hpp"
#include "log/log_help.h"
#include "network/rpc/rpc_call.h"
#include "network/rpc/rpc_reply.h"
#include "network/rpc/rpc_function.hpp"
#include "network/message_meta.h"
#include "network/io_service_pool/io_service_pool.h"
#include "network/net/server.h"
#include "gbnet/buffer/buffer.h"
#include "network/md5.hpp"
#include "network/scheduler/executor.h"
#include "common/singleton.h"
#include "network/router/router.h"
#include <atomic>
#include <mutex>

NAMESPACE_BEGIN(gb)

class NetworkManager : public Singleton<NetworkManager>
{
public:
	NetworkManager() = default;
	~NetworkManager() = default;

	NetworkManager(const NetworkManager&) = delete;
	NetworkManager& operator=(const NetworkManager&) = delete;
public:
	using ListenMap = typename std::unordered_map<uint32_t, net_listen_fun>;
	using RpcInterfaceMap = typename std::map<uint64_t, rpc_listen_fun>;
	using RpcCallerMap = typename std::map<uint64_t, RpcCallPtr>;


	void Init(HandleInterface* handleInterface);

	void Send(Session* session, uint32_t type, uint64_t id, google::protobuf::Message& msg);
	void Send(std::shared_ptr<Session> session, uint32_t type, uint64_t id, google::protobuf::Message& msg);

	void ListenOption(uint32_t type, net_listen_fun f, std::string protoName);

	template <typename F>
	void Listen(uint32_t type, F f, std::string protoName = "")
	{
		net_listen_fun func;
		if constexpr (std::is_same_v<std::decay_t<F>, sol::function>)
			func = NetFunctionaTraits<sol::function>::make(f, protoName);
		else
			func = MakeNetHandler(std::move(f));
		ListenOption(type, std::move(func), protoName);
	}

	void UnListen(uint32_t type, std::string signal, int level = 0);

	virtual void Dispatch(const SessionPtr& session, const ReadBufferPtr& buffer, gb::Meta& meta, int meta_size, int64_t data_size);

	uint64_t GetSequence();

	void CallImpl(RpcCallPtr call, std::string method, sol::variadic_args& args);
	void CallImpl(gb::Meta& meta, RpcCallPtr call, const ReadBufferPtr buffer = nullptr);

	template<typename ...Args>
	void Call(RpcCallPtr call, std::string method, Args&&... args)
	{
		if (!call)
		{
			return;
		}
		Meta meta;
		uint64_t method_key = MD5::MD5Hash64(method.c_str());
		meta.method = method_key;
		uint64_t seq_id = GetSequence();
		meta.sequence = seq_id;
		meta.mode = MsgMode::Request;
		call->SetId(seq_id);
		if constexpr (sizeof...(args) > 0)
		{
			std::vector<uint8_t> data = gb::msgpack::pack<Args...>(std::forward<Args>(args)...);
			WriteBuffer          write_buffer;
			write_buffer.Append((const char*)data.data(), data.size());
			ReadBufferPtr read_buffer(new ReadBuffer());
			write_buffer.SwapOut(read_buffer.get());
			CallImpl(meta, call, read_buffer);
		}
		else
		{
			CallImpl(meta, call);
		}
	}

	void RegisterOption(std::string method, rpc_listen_fun fn);

	template<typename F>
	void Register(std::string method, F fn)
	{
		rpc_listen_fun func;
		if constexpr (std::is_same_v<std::decay_t<F>, sol::function>)
		{
			auto lua_state = fn.lua_state();
			sol::state_view lua_view(lua_state);
			sol::state* state = (sol::state*)&lua_view;
			func = RpcFunctionaTraits<sol::function>::make(state, fn);
		}
		else
		{
			func = MakeRpcHandler(std::move(fn));
		}
		RegisterOption(method, func);
	}

	void UnRegister(std::string method);
	void RpcCancel(int64_t seq_id);
	void OnReceiveCall(const SessionPtr& session, const ReadBufferPtr& buffer, int meta_size, int64_t data_size);
    Router& GetRouter() { return router_; }

private:
    WorkerExecutor      CreateExecutorForRoute(uint32_t type, uint64_t route_id) const;
    net_listen_fun FindListenFunction(uint32_t type);
    rpc_listen_fun FindRpcFunction(uint64_t method);

private:
	Router router_;
    ListenMap listen_function_map_{};
    RpcInterfaceMap rpc_interface_map_{};
    RpcCallerMap rpc_caller_map_{};
    std::mutex listen_mutex_;
    std::mutex rpc_interface_mutex_;
    std::mutex rpc_caller_mutex_;
    std::atomic<uint64_t> sequence_tail_{1};

};

NAMESPACE_END

#include "network/rpc/rpc_coro.h"
