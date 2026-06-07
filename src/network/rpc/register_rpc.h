#pragma once
#include <sol/sol.hpp>
#include <lua.hpp>
#include "worker/worker_manager.h"
#include "script/script.h"
#include <memory>

NAMESPACE_BEGIN(gb)

/// Move a sol::function from a coroutine lua_State to the Worker's main state.
/// Prevents conflicts when a callback captured inside a coroutine fires later
/// on the Worker thread (coroutine.yield/resume incompatibility).
/// Mirror of the redis LuaCbBridge pattern in register_redis.cpp.
inline sol::function BridgeCallback(sol::function func)
{
    auto worker = WorkerManager::Instance()->GetCurWorker();
    if (!worker)
        return func;
    lua_State* main_L = worker->GetScript()->lua_state();
    lua_State* cb_L   = func.lua_state();
    if (!main_L || !cb_L || main_L == cb_L)
        return func;
    func.push();
    lua_xmove(cb_L, main_L, 1);
    sol::function main_func(main_L, lua_gettop(main_L));
    lua_pop(main_L, 1);
    return main_func;
}

/// Register RPC Lua bindings (net.Register, net.Call, RpcCall, RpcReply, net.Await).
/// Must be called after register_net() creates the global "net" table.
void RegisterRpcLua(std::shared_ptr<Script>& scriptPtr);

NAMESPACE_END
