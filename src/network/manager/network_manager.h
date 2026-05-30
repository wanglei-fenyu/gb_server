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
		// sequence鍜宑all->SetId鍦–allImpl(Meta&, RpcCallPtr, ReadBufferPtr)鍐呴儴澶勭悊
		// 鐢ㄤ簬灏唚orker_index + local_seq缂栫爜鍒?4浣峴equence瀛楁涓?
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

    /// 鍐荤粨鎵€鏈夊鐞嗗櫒鏄犲皠涓哄彧璇诲師瀛愬揩鐓?
    /// 蹇呴』鍦ㄦ墍鏈塛orker鐨処nitLua()鍜屾墍鏈塕egister/Listen璋冪敤涔嬪悗璋冪敤
    /// 浣嗗繀椤诲湪浠讳綍缃戠粶娑堟伅鍒嗗彂涔嬪墠璋冪敤锛堟甯告祦绋嬩腑锛?
    /// 璋冪敤Freeze()鍚庯紝FindListenFunction鍜孎indRpcFunction鍙樹负鏃犻攣
    void Freeze();

private:
    WorkerExecutor      CreateExecutorForRoute(uint32_t type, uint64_t route_id) const;
    net_listen_fun FindListenFunction(uint32_t type);
    rpc_listen_fun FindRpcFunction(uint64_t method);

private:
	Router router_;
    ListenMap listen_function_map_{};
    RpcInterfaceMap rpc_interface_map_{};
    std::mutex listen_mutex_;
    std::mutex rpc_interface_mutex_;

    /// 鍐荤粨鐨勫彧璇诲揩鐓?鈥?鐢盕reeze()濉厖涓€娆★紝涔嬪悗鏃犻攣璇诲彇
    /// 鐢熷懡鍛ㄦ湡锛氬湪Freeze()鏈熼棿鍒嗛厤涓€娆★紝姘镐笉閲婃斁锛堟湁鎰忎负涔?鈥?杩涚▼鐢熷懡鍛ㄦ湡锛?
    std::atomic<const ListenMap*>    frozen_listen_map_{nullptr};
    std::atomic<const RpcInterfaceMap*> frozen_rpc_interface_map_{nullptr};

};

NAMESPACE_END

#include "network/rpc/rpc_coro.h"
