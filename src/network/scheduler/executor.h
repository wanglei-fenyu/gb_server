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
    explicit Executor(WorkerWeakPtr worker) :
        worker_(std::move(worker))
    {
    }

    static Executor Current()
    {
        return Executor(WorkerManager::Instance()->GetCurWorker());
    }

    static Executor Worker(WorkerWeakPtr worker)
    {
        return Executor(std::move(worker));
    }

    bool HasWorker() const
    {
        return !worker_.expired();
    }

    bool IsCurrent() const
    {
        auto worker = worker_.lock();
        if (!worker)
            return true;
        auto current = WorkerManager::Instance()->GetCurWorker();
        return current && current->GetIndex() == worker->GetIndex();
    }

    void Dispatch(std::function<void()> fn) const
    {
        if (!fn)
            return;

        auto worker = worker_.lock();
        if (!worker || IsCurrent())
        {
            fn();
            return;
        }
        worker->Post(std::move(fn));
    }

private:
    WorkerWeakPtr worker_;
};

} // namespace gb
