#pragma once
#include <functional>
#include <type_traits>
#include "session/session.h"
#include <sol/sol.hpp>
#include <lua.hpp>
#include "script/script.h"
#include "network/message_meta.h"
#include "gbnet/buffer/compressed_stream.h"
#include "rpc/rpc_function_help.h"



namespace gb
{


//Listener
typedef std::function<void(const SessionPtr& session, const ReadBufferPtr& buffer, Meta& meta,int meta_size,int64_t data_size)> net_listen_fun;


// ---------------------------------------------------------------------------
// MakeNetHandler — unified network message handler factory
//
// Supports any callable with 0, 1, or 2 parameters:
//   • 0 params                     : fn()
//   • (SessionPtr)                 : fn(session)
//   • (ProtoMsg)                   : fn(msg)          — parsed with compression
//   • (SessionPtr, ProtoMsg)       : fn(session, msg) — parsed with compression
//   • (ProtoMsg0, ProtoMsg1)       : fn(meta, msg0)   — parsed with compression
// ---------------------------------------------------------------------------
template <typename F>
net_listen_fun MakeNetHandler(F fn)
{
using ArgsTuple = typename function_traits<std::decay_t<F>>::args_tuple;
constexpr std::size_t N = std::tuple_size_v<ArgsTuple>;

return [fn](const SessionPtr& session, const ReadBufferPtr& buffer, Meta& meta, int meta_size, int64_t data_size) mutable -> void {
if constexpr (N == 0)
{
fn();
}
else if constexpr (N == 1)
{
using P0 = std::tuple_element_t<0, ArgsTuple>;
if constexpr (std::is_same_v<P0, std::shared_ptr<Session>>)
{
fn(session);
}
else if constexpr (std::is_base_of_v<google::protobuf::Message, P0>)
{
P0 p0;
if (meta.compress_type == CompressType::CompressTypeNone)
{
if (p0.ParsePartialFromZeroCopyStream(buffer.get()))
fn(p0);
}
else
{
std::shared_ptr<AbstractCompressedInputStream> is(gb::get_compressed_input_stream(buffer.get(), meta.compress_type));
if (p0.ParsePartialFromZeroCopyStream(is.get()))
fn(p0);
}
}
}
else if constexpr (N == 2)
{
using P0 = std::tuple_element_t<0, ArgsTuple>;
using P1 = std::tuple_element_t<1, ArgsTuple>;
if constexpr (std::is_same_v<P0, std::shared_ptr<Session>> && std::is_base_of_v<google::protobuf::Message, P1>)
{
P1 p1;
if (meta.compress_type == CompressType::CompressTypeNone)
{
if (p1.ParsePartialFromZeroCopyStream(buffer.get()))
fn(session, std::move(p1));
}
else
{
std::shared_ptr<AbstractCompressedInputStream> is(gb::get_compressed_input_stream(buffer.get(), meta.compress_type));
if (p1.ParsePartialFromZeroCopyStream(is.get()))
fn(session, std::move(p1));
}
}
else if constexpr (std::is_base_of_v<google::protobuf::Message, P0> && std::is_base_of_v<google::protobuf::Message, P1>)
{
P0 p0;
if (meta.compress_type == CompressType::CompressTypeNone)
{
if (p0.ParsePartialFromZeroCopyStream(buffer.get()))
fn(meta, std::move(p0));
}
else
{
std::shared_ptr<AbstractCompressedInputStream> is(get_compressed_input_stream(buffer.get(), meta.compress_type));
if (p0.ParsePartialFromZeroCopyStream(is.get()))
fn(meta, std::move(p0));
}
}
}
};
}


// ---------------------------------------------------------------------------
// NetFunctionaTraits — kept only for the sol::function (Lua) specialisation
// ---------------------------------------------------------------------------
template <class Fn, class F = Fn>
struct NetFunctionaTraits
{
static net_listen_fun make(F fn)
{
return nullptr;
}
};

template<>
struct NetFunctionaTraits<sol::function, sol::function>
{
static net_listen_fun make(sol::function fn, std::string_view protoName)
{
auto lua_state = fn.lua_state();
sol::state_view lua_view(lua_state);
return [lua_view, fn, proto = std::string(protoName)](const SessionPtr& session, const ReadBufferPtr& buffer, Meta& meta, int meta_size, int64_t data_size) {
if (proto.empty())
{
fn(session);
return;
}
int  top        = lua_gettop(lua_view.lua_state());
auto create_msg = lua_view["create_msg"];

if (!create_msg.valid())
{
lua_settop(lua_view.lua_state(), top);
return;
}

sol::object lua_messgae = create_msg(proto);
google::protobuf::Message* msg = lua_messgae.as<google::protobuf::Message*>();
if (!msg)
{
lua_settop(lua_view.lua_state(), top);
return;
}
if (meta.compress_type == CompressType::CompressTypeNone)
{
if (msg->ParsePartialFromZeroCopyStream(buffer.get()))
fn(session, lua_messgae);
}
else
{
std::shared_ptr<AbstractCompressedInputStream> is(get_compressed_input_stream(buffer.get(), meta.compress_type));
if (msg->ParsePartialFromZeroCopyStream(is.get()))
fn(session, lua_messgae);
}
lua_settop(lua_view.lua_state(), top);
};
}
};


template <typename T>
class HasInvokeOperator {
typedef char yes;
typedef struct {
char a[2];
} no;
template <typename TT>
static yes HasFunc(decltype(&TT::operator()));
template <typename TT>
static no HasFunc(...);

public:
static const bool value = sizeof(HasFunc<T>(nullptr)) == sizeof(yes);
};


// ---------------------------------------------------------------------------
// ServerLambdaFunc — backward-compatible helper, delegates to MakeNetHandler
// ---------------------------------------------------------------------------
template <typename T, typename F>
static net_listen_fun ServerLambdaFunc(T lambda, F)
{
return MakeNetHandler(std::move(lambda));
}

}
