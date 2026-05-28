#pragma once
#include "common/worker/worker_manager.h"
#include "async_simple/Executor.h"
#include <functional>
#include <memory>
#include <utility>

namespace gb
{

class WorkerExecutor;

class GbAsyncExecutor : public async_simple::Executor
{
public:
    explicit GbAsyncExecutor(const std::shared_ptr<WorkerExecutor>& owner) :
        async_simple::Executor("gb_executor"), executor_(owner)
    {
    }

    bool schedule(Func func) override;
    bool currentThreadInExecutor() const override;
    async_simple::ExecutorStat stat() const override { return {}; }
    async_simple::IOExecutor*  getIOExecutor() override { return nullptr; }

private:
    std::weak_ptr<WorkerExecutor> executor_{};
};

class WorkerExecutor
{
public:
    using Func = std::function<void()>;

    WorkerExecutor() = default;
    WorkerExecutor(WorkerWeakPtr worker, bool inline_fallback = true) :
        worker_(std::move(worker)), inline_fallback_(inline_fallback)
    {
    }
    WorkerExecutor(std::function<void(Func)> dispatch, std::function<bool()> in_executor, bool inline_fallback = true) :
        dispatch_(std::move(dispatch)),
        in_executor_(std::move(in_executor)),
        inline_fallback_(inline_fallback)
    {
    }

    WorkerExecutor(const WorkerExecutor&) = default;
    WorkerExecutor& operator=(const WorkerExecutor&) = default;
    WorkerExecutor(WorkerExecutor&&) noexcept = default;
    WorkerExecutor& operator=(WorkerExecutor&&) noexcept = default;

    static WorkerExecutor Main(bool inline_fallback = true)
    {
        return WorkerExecutor(
            [](Func fn) { WorkerManager::Instance()->PostMain(std::move(fn)); },
            []() { return WorkerManager::Instance()->IsMainThread(); },
            inline_fallback);
    }

    static WorkerExecutor Current(bool inline_fallback = true)
    {
        return WorkerExecutor(WorkerManager::Instance()->GetCurWorker(), inline_fallback);
    }

    static WorkerExecutor Worker(WorkerWeakPtr worker, bool inline_fallback = true)
    {
        return WorkerExecutor(std::move(worker), inline_fallback);
    }

    bool HasWorker() const
    {
        return !worker_.expired();
    }

    bool IsCurrent() const
    {
        auto worker = worker_.lock();
        if (worker)
        {
            auto current = WorkerManager::Instance()->GetCurWorker();
            return current && current->GetIndex() == worker->GetIndex();
        }
        if (in_executor_)
            return in_executor_();
        return false;
    }

    bool schedule(Func fn)
    {
        return Dispatch(std::move(fn));
    }

    bool currentThreadInExecutor() const
    {
        return IsCurrent();
    }

    bool Dispatch(Func fn) const
    {
        if (!fn)
            return false;

        auto worker = worker_.lock();
        if (worker)
        {
            if (IsCurrent())
            {
                fn();
                return true;
            }
            worker->Post(std::move(fn));
            return true;
        }
        if (dispatch_)
        {
            if (IsCurrent())
            {
                fn();
                return true;
            }
            dispatch_(std::move(fn));
            return true;
        }
        if (inline_fallback_)
        {
            fn();
            return true;
        }
        return false;
    }

private:
    WorkerWeakPtr worker_;
    std::function<void(Func)> dispatch_;
    std::function<bool()>     in_executor_;
    bool                      inline_fallback_{true};
};

inline bool GbAsyncExecutor::schedule(Func func)
{
    auto executor = executor_.lock();
    return executor ? executor->Dispatch(std::move(func)) : false;
}

inline bool GbAsyncExecutor::currentThreadInExecutor() const
{
    auto executor = executor_.lock();
    return executor ? executor->IsCurrent() : false;
}

} // namespace gb
