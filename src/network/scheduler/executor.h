#pragma once
#include "common/worker/worker_manager.h"
#include "async_simple/Executor.h"
#include <functional>
#include <utility>

namespace gb
{

class Executor : public async_simple::Executor
{
public:
    Executor() : async_simple::Executor("gb_executor") {}
    Executor(WorkerWeakPtr worker, bool inline_fallback = true) :
        async_simple::Executor("gb_executor"), worker_(std::move(worker)), inline_fallback_(inline_fallback)
    {
    }
    Executor(std::function<void(Func)> dispatch, std::function<bool()> in_executor, bool inline_fallback = true) :
        async_simple::Executor("gb_executor"),
        dispatch_(std::move(dispatch)),
        in_executor_(std::move(in_executor)),
        inline_fallback_(inline_fallback)
    {
    }

    Executor(const Executor& rhs) :
        async_simple::Executor("gb_executor"),
        worker_(rhs.worker_),
        dispatch_(rhs.dispatch_),
        in_executor_(rhs.in_executor_),
        inline_fallback_(rhs.inline_fallback_)
    {
    }

    Executor& operator=(const Executor& rhs)
    {
        if (this == &rhs)
            return *this;
        worker_          = rhs.worker_;
        dispatch_        = rhs.dispatch_;
        in_executor_     = rhs.in_executor_;
        inline_fallback_ = rhs.inline_fallback_;
        return *this;
    }

    Executor(Executor&& rhs) noexcept :
        async_simple::Executor("gb_executor"),
        worker_(std::move(rhs.worker_)),
        dispatch_(std::move(rhs.dispatch_)),
        in_executor_(std::move(rhs.in_executor_)),
        inline_fallback_(rhs.inline_fallback_)
    {
    }

    Executor& operator=(Executor&& rhs) noexcept
    {
        if (this == &rhs)
            return *this;
        worker_          = std::move(rhs.worker_);
        dispatch_        = std::move(rhs.dispatch_);
        in_executor_     = std::move(rhs.in_executor_);
        inline_fallback_ = rhs.inline_fallback_;
        return *this;
    }

    static Executor Current(bool inline_fallback = true)
    {
        return Executor(WorkerManager::Instance()->GetCurWorker(), inline_fallback);
    }

    static Executor Worker(WorkerWeakPtr worker, bool inline_fallback = true)
    {
        return Executor(std::move(worker), inline_fallback);
    }

    static Executor Main(bool inline_fallback = true)
    {
        return Executor(
            [](Func fn) { WorkerManager::Instance()->PostMain(std::move(fn)); },
            []() { return WorkerManager::Instance()->IsMainThread(); },
            inline_fallback);
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

    bool schedule(Func fn) override
    {
        return Dispatch(std::move(fn));
    }

    bool currentThreadInExecutor() const override
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
    bool          inline_fallback_{true};
};

} // namespace gb
