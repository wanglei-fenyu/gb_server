#include "router.h"
#include "worker/worker_manager.h"

namespace gb
{

	Router::~Router()
	{
		delete frozen_.load(std::memory_order_acquire);
	}

	void Router::RegisterWorker(ServiceWorkerType service_worker_type, WorkerWeakPtr worker)
	{
		route_table_.RegisterWorker(service_worker_type, worker);
	}

    void Router::SetServiceTypeResolver(std::function<ServiceWorkerType(MessageType)> resolver)
    {
        route_table_.SetServiceTypeResolver(std::move(resolver));
    }

    void Router::SetRouteKeySelector(std::function<uint64_t(MessageType, uint64_t)> selector)
    {
        std::lock_guard<std::mutex> lock(strategy_mutex_);
        route_key_selector_ = std::move(selector);
    }

    void Router::SetWorkerIndexSelector(std::function<size_t(const std::vector<WorkerWeakPtr>&, MessageType, uint64_t)> selector)
    {
        std::lock_guard<std::mutex> lock(strategy_mutex_);
        worker_index_selector_ = std::move(selector);
    }

    void Router::Freeze()
    {
        if (frozen_.load(std::memory_order_acquire))
            return;

        auto* snapshot = new Snapshot();
        snapshot->route_table = route_table_.CaptureSnapshot();
        {
            std::lock_guard<std::mutex> lock(strategy_mutex_);
            snapshot->route_key_selector    = route_key_selector_;
            snapshot->worker_index_selector = worker_index_selector_;
        }
        frozen_.store(snapshot, std::memory_order_release);
    }

    WorkerWeakPtr Router::PickWorker(
        const std::vector<WorkerWeakPtr>& workers,
        const std::function<size_t(const std::vector<WorkerWeakPtr>&, MessageType, uint64_t)>& worker_index_selector,
        const std::function<uint64_t(MessageType, uint64_t)>& route_key_selector,
        MessageType message_type, uint64 route_id) const
    {
        if (workers.empty())
            return {};

        if (worker_index_selector)
        {
            size_t index = worker_index_selector(workers, message_type, route_id);
            if (index < workers.size())
                return workers[index];
            return {};
        }

        uint64_t route_key = route_id;
        if (route_key_selector)
            route_key = route_key_selector(message_type, route_id);
        size_t index = route_key % workers.size();
        return workers[index];
    }

    WorkerExecutor Router::ToExecutor(WorkerWeakPtr target_weak) const
    {
        auto target = target_weak.lock();
        if (!target)
        {
            auto main_worker = WorkerManager::Instance()->GetMainWorker();
            if (main_worker)
            {
                auto executor = main_worker->GetExecutor();
                if (executor)
                    return *executor;
                return WorkerExecutor::Worker(main_worker);
            }
            return WorkerExecutor::Main();
        }
        auto executor = target->GetExecutor();
        if (!executor)
            return WorkerExecutor::Worker(target);
        return *executor;
    }

	WorkerExecutor Router::GetServiceExecutor(MessageType message_type, uint64 route_id) const
	{
        // 快速路径：冻结后从只读快照读取——无锁、无拷贝策略对象。
        const Snapshot* snapshot = frozen_.load(std::memory_order_acquire);
        if (snapshot)
        {
            ServiceWorkerType swt = RouteTable::ResolveServiceWorkerType(snapshot->route_table, message_type);
            if (swt >= snapshot->route_table.workers.size())
                return ToExecutor({});
            const auto& workers = snapshot->route_table.workers[swt];
            auto target = PickWorker(workers, snapshot->worker_index_selector, snapshot->route_key_selector, message_type, route_id);
            return ToExecutor(std::move(target));
        }

        // 回退路径：冻结前（注册阶段）——加锁读取，保持原有行为。
        std::function<size_t(const std::vector<WorkerWeakPtr>&, MessageType, uint64_t)> worker_index_selector;
        std::function<uint64_t(MessageType, uint64_t)> route_key_selector;
        {
            std::lock_guard<std::mutex> lock(strategy_mutex_);
            worker_index_selector = worker_index_selector_;
            route_key_selector    = route_key_selector_;
        }
        auto workers = route_table_.GetWorker(route_table_.ResolveServiceWorkerType(message_type));
        auto target  = PickWorker(workers, worker_index_selector, route_key_selector, message_type, route_id);
        return ToExecutor(std::move(target));
	}
}
