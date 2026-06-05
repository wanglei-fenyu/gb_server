#pragma once 
#include "gbnet/common/def.h"
#include "gbnet/common/define.h"
#include "network/io/session.h"
#include "network/rpc/function.hpp"
#include "log/log.h"
#include "network/rpc/rpc_call.h"
#include "network/rpc/rpc_reply.h"
#include "network/rpc/rpc_function.hpp"
#include "network/io/message_meta.h"
#include "network/io/io_service_pool.h"
#include "network/io/server.h"
#include "gbnet/buffer/buffer.h"
#include "base/md5.hpp"
#include "network/rpc/executor.h"
#include "base/singleton.h"
#include "network/router/router.h"
#include <atomic>
#include <mutex>

NAMESPACE_BEGIN(gb)

class NetworkManager : public Singleton<NetworkManager>
{
public:
	NetworkManager() = default;
	~NetworkManager();

	NetworkManager(const NetworkManager&) = delete;
	NetworkManager& operator=(const NetworkManager&) = delete;
public:
	using ListenMap = typename std::unordered_map<uint32_t, net_listen_fun>;
	using RpcInterfaceMap = typename std::map<uint64_t, rpc_listen_fun>;


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

	void CallImpl(RpcCallPtr call, std::string method, sol::variadic_args& args);
	void CallImpl(gb::Meta& meta, RpcCallPtr call, const ReadBufferPtr buffer = nullptr);

	template<typename ...Args>
	void Call(RpcCallPtr call, std::string method, Args&&... args)
	{
		if (!call)
			return;
		Meta meta;
		uint64_t method_key = MD5::MD5Hash64(method.c_str());
		meta.method = method_key;
		meta.mode = MsgMode::Request;
		// sequence和call->SetId在CallImpl(Meta&, RpcCallPtr, ReadBufferPtr)内部处理
		// 用于将worker_index + local_seq编码到64位sequence字段中
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
	void OnReceiveCall(const SessionPtr& session, const ReadBufferPtr& buffer, int meta_size, int64_t data_size);
    Router& GetRouter() { return router_; }

    /// 冻结所有处理器映射为只读原子快照
    /// 必须在所有Worker的InitLua()和所有Register/Listen调用之后调用
    /// 但必须在任何网络消息分发之前调用（正常流程中）
    /// 调用Freeze()后，FindListenFunction和FindRpcFunction变为无锁
    void Freeze();

private:
    net_listen_fun FindListenFunction(uint32_t type);
    rpc_listen_fun FindRpcFunction(uint64_t method);

private:
	Router router_;
    ListenMap listen_function_map_{};
    RpcInterfaceMap rpc_interface_map_{};
    std::mutex listen_mutex_;
    std::mutex rpc_interface_mutex_;

    /// 冻结的只读快照——由Freeze()填充一次，之后无锁读取
    /// 生命周期：在Freeze()期间分配一次，永不释放（有意为之——进程生命周期）
    std::atomic<const ListenMap*>    frozen_listen_map_{nullptr};
    std::atomic<const RpcInterfaceMap*> frozen_rpc_interface_map_{nullptr};

};

NAMESPACE_END

#include "network/rpc/rpc_coro.h"
