#include "script/register_script.h"
#include "log/log.h"
#include "network/etcd/etcd_manager.h"
#include "worker/worker_manager.h"

namespace
{
class LuaCbBridge
{
public:
    static std::shared_ptr<LuaCbBridge> Create(const gb::WorkerPtr& worker, sol::function cb)
    {
        auto bridge = std::make_shared<LuaCbBridge>();
        bridge->worker_ = worker;

        lua_State* main_state = worker ? worker->GetScript()->lua_state() : nullptr;
        if (main_state)
        {
            lua_State* callback_state = cb.lua_state();
            if (callback_state && callback_state != main_state)
            {
                cb.push();
                lua_xmove(callback_state, main_state, 1);
                sol::function moved(main_state, lua_gettop(main_state));
                lua_pop(main_state, 1);
                bridge->callback_ = std::make_shared<sol::function>(std::move(moved));
                return bridge;
            }
        }

        bridge->callback_ = std::make_shared<sol::function>(std::move(cb));
        return bridge;
    }

    void PostCb(int err)
    {
        auto worker = worker_.lock();
        if (!worker)
            return;

        auto callback = callback_;
        worker->Post([callback, err]() {
            if (callback && callback->valid())
                (*callback)(err == gb::EtcdError::OK ? "" : "etcd request failed");
        });
    }

    template<typename T>
    void PostCb(int err, T value)
    {
        auto worker = worker_.lock();
        if (!worker)
            return;

        auto callback = callback_;
        worker->Post([callback, err, value = std::move(value)]() mutable {
            if (callback && callback->valid())
                (*callback)(err == gb::EtcdError::OK ? "" : "etcd request failed", std::move(value));
        });
    }

private:
    gb::WorkerWeakPtr worker_;
    std::shared_ptr<sol::function> callback_;
};
} // namespace

using namespace gb;

void register_etcd(std::shared_ptr<Script>& scriptPtr)
{
    auto etcd = scriptPtr->create_table("etcd");

    etcd["Connect"] = [](const std::string& endpoint) -> int {
        return EtcdManager::Instance()->Connect(endpoint);
    };

    etcd["Disconnect"] = []() {
        EtcdManager::Instance()->Disconnect();
    };

    etcd["IsConnected"] = []() -> bool {
        return EtcdManager::Instance()->IsConnected();
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 异步回调接口
    //
    // 统一约定: callback(err, [val...])
    //   err = "" 表示成功，非空表示失败
    //   回调会被派发回发起调用的 Worker 线程
    // ═══════════════════════════════════════════════════════════════════════

    etcd["AsyncPut"] = [](const std::string& key,
                           const std::string& value,
                           sol::function callback) {
        auto worker = WorkerManager::Instance()->GetCurWorker();
        auto bridge = LuaCbBridge::Create(worker, std::move(callback));
        EtcdManager::Instance()->AsyncPut(key, value, [bridge](int rc) {
            bridge->PostCb(rc);
        });
    };

    etcd["AsyncPutWithLease"] = [](const std::string& key,
                                    const std::string& value,
                                    int64_t lease_id,
                                    sol::function callback) {
        auto worker = WorkerManager::Instance()->GetCurWorker();
        auto bridge = LuaCbBridge::Create(worker, std::move(callback));
        EtcdManager::Instance()->AsyncPutWithLease(key, value, lease_id, [bridge](int rc) {
            bridge->PostCb(rc);
        });
    };

    etcd["AsyncPutWithTTL"] = [](const std::string& key,
                                  const std::string& value,
                                  int ttl_seconds,
                                  sol::function callback) {
        auto worker = WorkerManager::Instance()->GetCurWorker();
        auto bridge = LuaCbBridge::Create(worker, std::move(callback));
        EtcdManager::Instance()->AsyncPutWithTTL(key, value, ttl_seconds, [bridge](int rc) {
            bridge->PostCb(rc);
        });
    };

    etcd["AsyncGet"] = [](const std::string& key, sol::function callback) {
        auto worker = WorkerManager::Instance()->GetCurWorker();
        auto bridge = LuaCbBridge::Create(worker, std::move(callback));
        EtcdManager::Instance()->AsyncGet(key, [bridge](int rc, std::string value) {
            bridge->PostCb(rc, std::move(value));
        });
    };

    etcd["AsyncDelete"] = [](const std::string& key, sol::function callback) {
        auto worker = WorkerManager::Instance()->GetCurWorker();
        auto bridge = LuaCbBridge::Create(worker, std::move(callback));
        EtcdManager::Instance()->AsyncDelete(key, [bridge](int rc) {
            bridge->PostCb(rc);
        });
    };

    etcd["AsyncGrantLease"] = [](int ttl_seconds, sol::function callback) {
        auto worker = WorkerManager::Instance()->GetCurWorker();
        auto bridge = LuaCbBridge::Create(worker, std::move(callback));
        EtcdManager::Instance()->AsyncGrantLease(ttl_seconds, [bridge](int rc, int64_t lease_id) {
            bridge->PostCb(rc, lease_id);
        });
    };

    // ═══════════════════════════════════════════════════════════════════════
    // 同步接口
    // ═══════════════════════════════════════════════════════════════════════

    etcd["Put"] = [](const std::string& key, const std::string& value) -> int {
        return EtcdManager::Instance()->Put(key, value);
    };

    etcd["PutWithLease"] = [](const std::string& key, const std::string& value, int64_t lease_id) -> int {
        return EtcdManager::Instance()->PutWithLease(key, value, lease_id);
    };

    etcd["PutWithTTL"] = [](const std::string& key, const std::string& value, int ttl_seconds) -> int {
        return EtcdManager::Instance()->PutWithTTL(key, value, ttl_seconds);
    };

    etcd["Get"] = [scriptPtr](const std::string& key) -> sol::object {
        std::string value;
        int rc = EtcdManager::Instance()->Get(key, value);
        if (rc != EtcdError::OK)
            return sol::make_object(scriptPtr->lua_state(), sol::nil);
        return sol::make_object(scriptPtr->lua_state(), value);
    };

    etcd["Delete"] = [](const std::string& key) -> int {
        return EtcdManager::Instance()->Delete(key);
    };

    etcd["GrantLease"] = [scriptPtr](int ttl_seconds) -> sol::object {
        int64_t lease_id = 0;
        int rc = EtcdManager::Instance()->GrantLease(ttl_seconds, lease_id);
        if (rc != EtcdError::OK)
            return sol::make_object(scriptPtr->lua_state(), sol::nil);
        return sol::make_object(scriptPtr->lua_state(), lease_id);
    };

    // Watch(key, callback[, interval_ms]) -> watch_id | nil
    // callback(event): event = { key=..., value=..., deleted=true|false, type="put"|"delete" }
    etcd["Watch"] = [scriptPtr](const std::string& key, sol::function callback, sol::optional<int> interval_ms) -> sol::object {
        if (!callback.valid())
            return sol::make_object(scriptPtr->lua_state(), sol::nil);

        auto worker = WorkerManager::Instance()->GetCurWorker();
        if (!worker)
        {
            LOG_ERROR("[Lua][etcd] Watch must be called on a worker thread");
            return sol::make_object(scriptPtr->lua_state(), sol::nil);
        }

        const auto worker_index = worker->GetIndex();
        auto callback_ptr = std::make_shared<sol::function>(std::move(callback));

        int watch_id = EtcdManager::Instance()->Watch(
            key,
            [worker_index, callback_ptr](const std::string& event_key,
                                         const std::string& event_value,
                                         bool deleted) {
                WorkerManager::Instance()->PostToWorker(worker_index, [callback_ptr, event_key, event_value, deleted]() {
                    if (!callback_ptr->valid())
                        return;

                    sol::state_view lua(callback_ptr->lua_state());
                    sol::table event = lua.create_table();
                    event["key"] = event_key;
                    event["value"] = event_value;
                    event["deleted"] = deleted;
                    event["type"] = deleted ? "delete" : "put";
                    (*callback_ptr)(event);
                });
            },
            interval_ms.value_or(1000));

        if (watch_id <= 0)
            return sol::make_object(scriptPtr->lua_state(), sol::nil);

        return sol::make_object(scriptPtr->lua_state(), watch_id);
    };

    // Unwatch(watch_id) -> rc
    etcd["Unwatch"] = [](int watch_id) -> int {
        return EtcdManager::Instance()->Unwatch(watch_id);
    };

    etcd["Update"] = []() {
        EtcdManager::Instance()->Update();
    };

    // ═══════════════════════════════════════════════════════════════════════
    // Lua 协程桥接 — etcd.Await(method, ...)
    //
    // 用法：
    //   local err, val = etcd.Await("Get", "mykey")
    //   local err = etcd.Await("Put", "mykey", "value")
    // ═══════════════════════════════════════════════════════════════════════

    lua_State* L = scriptPtr->lua_state();
    luaL_dostring(L, R"(
        if not etcd.Await then
            function etcd.Await(method, ...)
                local co = coroutine.running()
                if not co then
                    error("etcd.Await() must be called from a coroutine")
                end

                local args = { ... }
                local results = nil
                local yielded = false

                local function cb(...)
                    results = { ... }
                    if yielded then
                        local ok, err = coroutine.resume(co)
                        if not ok then
                            error("etcd.Await() resume failed: " .. tostring(err))
                        end
                    end
                end

                args[#args + 1] = cb
                local async_fn = etcd["Async" .. method]
                if not async_fn then
                    error("Unknown async method: etcd.Async" .. method)
                end

                async_fn(table.unpack(args))

                if results == nil then
                    yielded = true
                    coroutine.yield()
                    yielded = false
                end

                return table.unpack(results)
            end
        end
    )");

    LOG_INFO("Etcd Lua API registered (async + await)");
}
