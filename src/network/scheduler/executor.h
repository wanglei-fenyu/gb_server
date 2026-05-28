#pragma once
#include "common/worker/worker_manager.h"
#include "async_simple/Executor.h"
#include <functional>
#include <utility>

namespace gb
{

class Executor;

class GbAsyncExecutor : public async_simple::Executor
{
public:
    explicit GbAsyncExecutor(Executor* owner) :
        async_simple::Executor("gb_executor"), owner_(owner)
    {
    }

    bool schedule(Func func) override;
    bool currentThreadInExecutor() const override;
    ExecutorStat stat() const override { return {}; }
    IOExecutor*  getIOExecutor() override { return nullptr; }

private:
    Executor* owner_{nullptr};
};

class Executor
{
public:
    using Func = std::function<void()>;

    Executor() = default;
    Executor(WorkerWeakPtr worker, bool inline_fallback = true) :
        worker_(std::move(worker)), inline_fallback_(inline_fallback)
    {
    }
    Executor(std::function<void(Func)> dispatch, std::function<bool()> in_executor, bool inline_fallback = true) :
        dispatch_(std::move(dispatch)),
        in_executor_(std::move(in_executor)),
        inline_fallback_(inline_fallback)
    {
    }

    Executor(const Executor&) = default;
    Executor& operator=(const Executor&) = default;
    Executor(Executor&&) noexcept = default;
    Executor& operator=(Executor&&) noexcept = default;

    static Executor Main(bool inline_fallback = true)
    {
        return Executor(
            [](Func fn) { WorkerManager::Instance()->PostMain(std::move(fn)); },
            []() { return WorkerManager::Instance()->IsMainThread(); },
            inline_fallback);
    }

    static Executor Current(bool inline_fallback = true)
    {
        return Executor(WorkerManager::Instance()->GetCurWorker(), inline_fallback);
    }

    static Executor Worker(WorkerWeakPtr worker, bool inline_fallback = true)
    {
        return Executor(std::move(worker), inline_fallback);
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
    return owner_ ? owner_->Dispatch(std::move(func)) : false;
}

inline bool GbAsyncExecutor::currentThreadInExecutor() const
{
    return owner_ ? owner_->IsCurrent() : false;
}

} // namespace gb
