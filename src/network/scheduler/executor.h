#pragma once
#include "common/worker/worker_manager.h"
#include <functional>
#include <utility>

namespace gb
{

class Executor
{
public:
    Executor() = default;
    Executor(WorkerWeakPtr worker, bool inline_fallback = true) :
        worker_(std::move(worker)), inline_fallback_(inline_fallback)
    {
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
        if (!worker)
            return false;
        auto current = WorkerManager::Instance()->GetCurWorker();
        return current && current->GetIndex() == worker->GetIndex();
    }

    void Dispatch(std::function<void()> fn) const
    {
        if (!fn)
            return;

        auto worker = worker_.lock();
        if (!worker)
        {
            if (inline_fallback_)
                fn();
            return;
        }
        if (IsCurrent())
        {
            fn();
            return;
        }
        worker->Post(std::move(fn));
    }

private:
    WorkerWeakPtr worker_;
    bool          inline_fallback_{true};
};

} // namespace gb
