#pragma once 
#include <functional>
#include <type_traits>
#include "network/io/session.h"
#include <sol/sol.hpp>
#include <lua.hpp>
#include "script/script.h"
#include "network/io/message_meta.h"
#include "rpc_reply.h"
#include "msgpack/msgpack.hpp"
#include <gbnet/buffer/compressed_stream.h>
#include "rpc_function_help.h"
#include "register_rpc.h"

namespace gb
{


typedef std::function<void(const SessionPtr& session, const ReadBufferPtr& buffer, gb::Meta& meta, int meta_size, int64_t data_size)> rpc_listen_fun;


// ---------------------------------------------------------------------------
// msgpack_unpack_tuple<Tuple>(data, size) — 通过msgpack解包元组
// ---------------------------------------------------------------------------
namespace detail
{
	template <typename Tuple, std::size_t... Is>
	auto msgpack_unpack_tuple_impl(const uint8_t* data, std::size_t size, std::index_sequence<Is...>)
	{
		return gb::msgpack::unpack<std::tuple_element_t<Is, Tuple>...>(data, size);
	}
} // namespace detail

template <typename Tuple>
auto msgpack_unpack_tuple(const uint8_t* data, std::size_t size)
{
	return detail::msgpack_unpack_tuple_impl<Tuple>(
		data, size, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}


// ---------------------------------------------------------------------------
// MakeRpcHandler — 统一的RPC处理器工厂
//
// 支持以下类型的可调用对象：
//   — RpcReply 作为第一个参数（可选，触发回复路由）
//   — google::protobuf::Message 派生类（从零拷贝流解析）
//   — 任何 msgpack 可序列化类型（通过 gb::msgpack::unpack 解包）
// ---------------------------------------------------------------------------
template <typename F>
rpc_listen_fun MakeRpcHandler(F f)
{
	using ArgsTuple = typename function_traits<std::decay_t<F>>::args_tuple;

	return [f](const SessionPtr& session, const ReadBufferPtr& buffer, gb::Meta& meta, int meta_size, int64_t data_size) mutable -> void {
		if constexpr (std::tuple_size_v<ArgsTuple> == 0)
		{
			f();
		}
		else
		{
			using P0 = std::tuple_element_t<0, ArgsTuple>;

			if constexpr (std::is_same_v<P0, RpcReply>)
			{
				if constexpr (std::tuple_size_v<ArgsTuple> == 1)
				{
					// f(RpcReply)
					f(RpcReply(std::move(meta), session));
				}
				else if constexpr (std::tuple_size_v<ArgsTuple> == 2 && std::is_base_of_v<google::protobuf::Message, std::tuple_element_t<1, ArgsTuple>>)
				{
					// f(RpcReply, ProtoMsg) — 从流中解析第二个参数
					using P1 = std::tuple_element_t<1, ArgsTuple>;
					P1 p1;
					if (p1.ParsePartialFromZeroCopyStream(buffer.get()))
						f(RpcReply(std::move(meta), session), std::move(p1));
				}
				else
				{
					// f(RpcReply, T1, T2, ...) — msgpack解包剩余参数
					using TailTuple = tuple_tail_t<1, ArgsTuple>;
					std::string s;
					GetMsgData(meta, buffer, meta_size, data_size, s);
					auto result = msgpack_unpack_tuple<TailTuple>((uint8_t*)s.data(), s.size());
					std::apply(
						[&](auto&&... args) {
							f(RpcReply(std::move(meta), session), std::forward<decltype(args)>(args)...);
						},
						std::move(result));
				}
			}
			else if constexpr (std::tuple_size_v<ArgsTuple> == 1 && std::is_base_of_v<google::protobuf::Message, P0>)
			{
				// f(ProtoMsg) — 单个protobuf参数
				P0 p0;
				if (p0.ParsePartialFromZeroCopyStream(buffer.get()))
					f(std::move(p0));
			}
			else
			{
				// f(T0, T1, ...) — msgpack解包所有参数
				std::string s;
				GetMsgData(meta, buffer, meta_size, data_size, s);
				auto result = msgpack_unpack_tuple<ArgsTuple>((uint8_t*)s.data(), s.size());
				std::apply(
					[&](auto&&... args) {
						f(std::forward<decltype(args)>(args)...);
					},
					std::move(result));
			}
		}
	};
}


// ---------------------------------------------------------------------------
// RpcFunctionaTraits — 仅保留用于 sol::function (Lua) 的特化
// ---------------------------------------------------------------------------
template <class Fn, class F = Fn>
struct RpcFunctionaTraits
{
	static rpc_listen_fun make(F fp)
	{
		static_assert("rpc function invalid");
		return nullptr;
	}
};

template <>
struct RpcFunctionaTraits<sol::function, sol::function>
{
	static rpc_listen_fun make(sol::state* state, sol::function func)
	{
        sol::function main_func = BridgeCallback(std::move(func));
        lua_State*    lstate    = main_func.lua_state();
        return [lstate, func = std::move(main_func)](const SessionPtr& session, const ReadBufferPtr& buffer, gb::Meta& meta, int meta_size, int64_t data_size) -> void {
            int         top = lua_gettop(lstate);
            std::string s   = "";
            GetMsgData(meta, buffer, meta_size, data_size, s);
            RpcReply reply(std::move(meta), session);
            sol::state_view lua_view(lstate);
            if (data_size > 0)
            {
                sol::variadic_args               arg = gb::msgpack::unpack(lua_view, (uint8_t*)s.data(), s.size());
				sol::protected_function_result result = func(reply, arg);
				if (!result.valid())
				{
					OnScriptError(result);
				}
			}
			else
			{
				sol::protected_function_result result = func(reply);
				if (!result.valid())
				{
					OnScriptError(result);
				}
			}
			lua_settop(lstate, top);
		};
	}
};


// ---------------------------------------------------------------------------
// RpcLambdaFunc — 向后兼容的辅助函数，委托给 MakeRpcHandler
// ---------------------------------------------------------------------------
template <typename T, typename F>
static rpc_listen_fun RpcLambdaFunc(T lambda, F)
{
	return MakeRpcHandler(std::move(lambda));
}

} // namespace gb