#include "script/script.h"
#include "timer/timer_manager.h"
#include "worker/worker_manager.h"
#include "log/log.h"

using namespace gb;

void register_timer(std::shared_ptr<Script>& scriptPtr)
{
    auto timer_table = scriptPtr->create_table("timer");

    timer_table["Register"] = [](long long milliseconds, sol::function callback, bool loop) -> long long {
        auto worker = WorkerManager::Instance()->GetCurWorker();
        if (!worker)
        {
            LOG_ERROR("[Lua] timer.Register failed: no current worker");
            return -1;
        }
        auto& timer_mgr = worker->GetTimerManager();
        if (!timer_mgr)
        {
            LOG_ERROR("[Lua] timer.Register failed: no timer manager");
            return -1;
        }

        // Use shared_ptr to let the callback know its own timer_id
        // (avoids Lua upvalue capture issues where closure captures nil)
        auto id_holder = std::make_shared<int64_t>(0);
        auto timer_id = timer_mgr->RegisterTimer(static_cast<int64_t>(milliseconds), [callback, id_holder]() {
            auto result = callback(static_cast<long long>(*id_holder));
            if (!result.valid())
            {
                sol::error err = result;
                LOG_ERROR("[Lua] timer callback error: {}", err.what());
            }
        }, loop);

        *id_holder = timer_id;
        return static_cast<long long>(timer_id);
    };

    timer_table["RegisterSystem"] = [](long long milliseconds, sol::function callback, bool loop) -> long long {
        auto worker = WorkerManager::Instance()->GetCurWorker();
        if (!worker)
        {
            LOG_ERROR("[Lua] timer.RegisterSystem failed: no current worker");
            return -1;
        }
        auto& timer_mgr = worker->GetTimerManager();
        if (!timer_mgr)
        {
            LOG_ERROR("[Lua] timer.RegisterSystem failed: no timer manager");
            return -1;
        }

        auto id_holder = std::make_shared<int64_t>(0);
        auto timer_id = timer_mgr->RegisterSystemTimer(static_cast<int64_t>(milliseconds), [callback, id_holder]() {
            auto result = callback(static_cast<long long>(*id_holder));
            if (!result.valid())
            {
                sol::error err = result;
                LOG_ERROR("[Lua] timer callback error: {}", err.what());
            }
        }, loop);

        *id_holder = timer_id;
        return static_cast<long long>(timer_id);
    };

    timer_table["UnRegister"] = [](long long timer_id) {
        auto worker = WorkerManager::Instance()->GetCurWorker();
        if (!worker)
        {
            LOG_ERROR("[Lua] timer.UnRegister failed: no current worker");
            return;
        }
        auto& timer_mgr = worker->GetTimerManager();
        if (!timer_mgr)
        {
            LOG_ERROR("[Lua] timer.UnRegister failed: no timer manager");
            return;
        }
        timer_mgr->UnRegisterTimer(static_cast<int64_t>(timer_id));
    };
}
