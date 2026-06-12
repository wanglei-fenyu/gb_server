#include "register_rpc.h"
#include "network/manager/network_manager.h"
#include "network/rpc/rpc_call.h"
#include "network/rpc/rpc_reply.h"
#include "network/io/session.h"
#include "network/io/message_meta.h"
#include "msgpack/msgpack.hpp"

NAMESPACE_BEGIN(gb)

void RegisterRpcLua(std::shared_ptr<Script>& scriptPtr)
{
    // Get the existing "net" table created by register_net()
    sol::table network = (*scriptPtr)["net"];

    // ── Server-side RPC handler registration ──
    network["Register"] = [](std::string method, sol::function f) {
        gb::NetworkManager::Instance()->Register(method, f);
    };

    // ── Client-side RPC call ──
    network["Call"] = sol::overload(
        [](RpcCallPtr call, std::string method, uint64_t id, sol::variadic_args args) {
            gb::NetworkManager::Instance()->CallImpl(call, method, id, args);
        },
        [](RpcCallPtr call, Meta meta, sol::variadic_args args) {
            if (args.size() > 0)
            {
                std::vector<uint8_t> data = gb::msgpack::pack(args);
                WriteBuffer          write_buffer;
                write_buffer.Append((const char*)data.data(), data.size());
                ReadBufferPtr read_buffer(new ReadBuffer());
                write_buffer.SwapOut(read_buffer.get());
                gb::NetworkManager::Instance()->CallImpl(meta, call, read_buffer);
            }
            else
            {
                gb::NetworkManager::Instance()->CallImpl(meta, call);
            }
        });

    // ── RpcCall usertype ──
    scriptPtr->new_usertype<RpcCall>("RpcCall",
        "new", sol::constructors<RpcCall>(),
        "SetSession", &RpcCall::SetSession,
        "SetCallBack", &RpcCall::SetCallBack<sol::function>,
        "SetTimeout", &RpcCall::SetTimeout,
        "Cancel", &RpcCall::Cancel,
        "SetId", &RpcCall::SetId,
        "GetId", &RpcCall::GetId);

    // ── RpcReply usertype ──
    scriptPtr->new_usertype<RpcReply>("RpcReply",
        "new", sol::constructors<
            RpcReply(Meta&, const std::shared_ptr<Session>&),
            RpcReply(Meta&&, const std::shared_ptr<Session>&)>(),
        "Invoke", sol::overload(
            static_cast<void(RpcReply::*)(sol::variadic_args)>(&RpcReply::Invoke)));

    // ── rpc table ──
    scriptPtr->create_table("rpc");

    // ── rpc.Await coroutine bridge ──
    // Call RPC from a Lua coroutine with (err_code, ...values) returns.
    // rpc.Await(method, id, setup, ...) -> err, result
    // setup is an optional function(call) to configure RpcCall before sending
    // (e.g. SetSession, SetTimeout, SetId, Cancel).
    lua_State* L = scriptPtr->lua_state();
    luaL_dostring(L, R"(
        function rpc.Await(method, id, setup, ...)
            if id == nil then id = 0 end
            local co = coroutine.running()
            if not co then
                error("rpc.Await() must be called from a coroutine")
            end
            local args = { ... }
            local results = nil
            local yielded = false
            local call = RpcCall.new()
            if setup then
                setup(call)
            end
            call:SetCallBack(function(reply, err, ...)
                results = { err, ... }
                if yielded then
                    coroutine.resume(co)
                end
            end)
            net.Call(call, method, id, table.unpack(args))
            if results == nil then
                yielded = true
                coroutine.yield()
                yielded = false
            end
            return table.unpack(results)
        end
    )");
}

NAMESPACE_END
