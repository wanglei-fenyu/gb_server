#pragma once
#include <functional>
#include <type_traits>
#include "network/io/session.h"
#include <sol/sol.hpp>
#include <lua.hpp>
#include "script/script.h"
#include "network/io/message_meta.h"
#include "gbnet/buffer/compressed_stream.h"
#include "network/rpc/rpc_function_help.h"
#include "network/rpc/register_rpc.h"



namespace gb
{


//Listener
typedef std::function<void(const SessionPtr& session, const ReadBufferPtr& buffer, Meta& meta, int meta_size, int64_t data_size)> net_listen_fun;


// ---------------------------------------------------------------------------
// MakeNetHandler — 统一的网络消息处理器工厂
//
// 支持0、1或2个参数的可调用对象：
//   — 0个参数                     : fn()
//   — (SessionPtr)                 : fn(session)
//   — (ProtoMsg)                   : fn(msg)          — 带压缩解析
//   — (SessionPtr, ProtoMsg)       : fn(session, msg) — 带压缩解析
//   — (ProtoMsg0, ProtoMsg1)       : fn(meta, msg0)   — 带压缩解析
// ---------------------------------------------------------------------------
template <typename F>
net_listen_fun MakeNetHandler(F fn)
{
    using ArgsTuple         = typename function_traits<std::decay_t<F>>::args_tuple;
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
// NetFunctionaTraits — 仅保留用于 sol::function (Lua) 的特化
// ---------------------------------------------------------------------------
template <class Fn, class F = Fn>
struct NetFunctionaTraits
{
    static net_listen_fun make(F fn)
    {
        return nullptr;
    }
};

template <>
struct NetFunctionaTraits<sol::function, sol::function>
{
    static net_listen_fun make(sol::function fn, std::string_view protoName)
    {
        sol::function main_func = BridgeCallback(std::move(fn));
        lua_State*    lstate    = main_func.lua_state();
        return [lstate, func = std::move(main_func), proto = std::string(protoName)](const SessionPtr& session, const ReadBufferPtr& buffer, Meta& meta, int meta_size, int64_t data_size) {
            if (proto.empty())
            {
                func(session);
                return;
            }
            int             top = lua_gettop(lstate);
            sol::state_view lua_view(lstate);
            auto            create_msg = lua_view["create_msg"];

            if (!create_msg.valid())
            {
                lua_settop(lstate, top);
                return;
            }

            sol::object                lua_messgae = create_msg(proto);
            google::protobuf::Message* msg         = lua_messgae.as<google::protobuf::Message*>();
            if (!msg)
            {
                lua_settop(lstate, top);
                return;
            }
            if (meta.compress_type == CompressType::CompressTypeNone)
            {
                if (msg->ParsePartialFromZeroCopyStream(buffer.get()))
                    func(session, lua_messgae);
            }
            else
            {
                std::shared_ptr<AbstractCompressedInputStream> is(get_compressed_input_stream(buffer.get(), meta.compress_type));
                if (msg->ParsePartialFromZeroCopyStream(is.get()))
                    func(session, lua_messgae);
            }
            lua_settop(lstate, top);
        };
    }
};


template <typename T>
class HasInvokeOperator
{
    typedef char yes;
    typedef struct
    {
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
// ServerLambdaFunc — 向后兼容的辅助函数，委托给 MakeNetHandler
// ---------------------------------------------------------------------------
template <typename T, typename F>
static net_listen_fun ServerLambdaFunc(T lambda, F)
{
    return MakeNetHandler(std::move(lambda));
}

} // namespace gb
