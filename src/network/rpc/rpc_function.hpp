#pragma once 
#include <functional>
#include <type_traits>
#include "network/session/session.h"
#include <sol/sol.hpp>
#include <lua.hpp>
#include "script/script.h"
#include "network/message_meta.h"
#include "rpc_reply.h"
#include "network/msgpack/msgpack.hpp"
#include <gbnet/buffer/compressed_stream.h>
#include "rpc_function_help.h"

namespace gb
{


typedef std::function<void(const SessionPtr& session, const ReadBufferPtr& buffer, gb::Meta& meta, int meta_size, int64_t data_size)> rpc_listen_fun;


// ---------------------------------------------------------------------------
// msgpack_unpack_tuple<Tuple>(data, size) — unpack a tuple of types via msgpack
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
// MakeRpcHandler — unified RPC handler factory
//
// Supports any callable with up to N parameters of the following kinds:
//   • RpcReply  as the first parameter (optional, triggers reply routing)
//   • google::protobuf::Message derivatives (parsed from the zero-copy stream)
//   • any msgpack-serialisable type (unpacked via gb::msgpack::unpack)
// ---------------------------------------------------------------------------
template <typename F>
rpc_listen_fun MakeRpcHandler(F f)
{
	using ArgsTuple = typename function_traits<std::decay_t<F>>::args_tuple;
	constexpr std::size_t N = std::tuple_size_v<ArgsTuple>;

	return [f](const SessionPtr& session, const ReadBufferPtr& buffer, gb::Meta& meta, int meta_size, int64_t data_size) mutable -> void {
		if constexpr (N == 0)
		{
			f();
		}
		else
		{
			using P0 = std::tuple_element_t<0, ArgsTuple>;

			if constexpr (std::is_same_v<P0, RpcReply>)
			{
				if constexpr (N == 1)
				{
					// f(RpcReply)
					f(RpcReply(std::move(meta), session));
				}
				else if constexpr (N == 2 && std::is_base_of_v<google::protobuf::Message, std::tuple_element_t<1, ArgsTuple>>)
				{
					// f(RpcReply, ProtoMsg) — parse second arg from stream
					using P1 = std::tuple_element_t<1, ArgsTuple>;
					P1 p1;
					if (p1.ParsePartialFromZeroCopyStream(buffer.get()))
						f(RpcReply(std::move(meta), session), std::move(p1));
				}
				else
				{
					// f(RpcReply, T1, T2, ...) — msgpack-unpack remaining args
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
			else if constexpr (N == 1 && std::is_base_of_v<google::protobuf::Message, P0>)
			{
				// f(ProtoMsg) — single protobuf arg
				P0 p0;
				if (p0.ParsePartialFromZeroCopyStream(buffer.get()))
					f(std::move(p0));
			}
			else
			{
				// f(T0, T1, ...) — msgpack-unpack all args
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
// RpcFunctionaTraits — kept only for the sol::function (Lua) specialisation
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
		return [state, func](const SessionPtr& session, const ReadBufferPtr& buffer, gb::Meta& meta, int meta_size, int64_t data_size) -> void {
			int         top = lua_gettop(state->lua_state());
			std::string s   = "";
			GetMsgData(meta, buffer, meta_size, data_size, s);
			RpcReply reply(std::move(meta), session);
			if (data_size > 0)
			{
				sol::variadic_args               arg = gb::msgpack::unpack(*state, (uint8_t*)s.data(), s.size());
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
			lua_settop(state->lua_state(), top);
		};
	}
};


// ---------------------------------------------------------------------------
// RpcLambdaFunc — backward-compatible helper, delegates to MakeRpcHandler
// ---------------------------------------------------------------------------
template <typename T, typename F>
static rpc_listen_fun RpcLambdaFunc(T lambda, F)
{
	return MakeRpcHandler(std::move(lambda));
}

} // namespace gb