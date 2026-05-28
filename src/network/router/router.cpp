#include "router.h"

namespace gb
{

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

    WorkerWeakPtr Router::PickWorker(const std::vector<WorkerWeakPtr>& workers, MessageType message_type, uint64 route_id) const
    {
        if (workers.empty())
            return {};

        std::function<size_t(const std::vector<WorkerWeakPtr>&, MessageType, uint64_t)> worker_index_selector;
        std::function<uint64_t(MessageType, uint64_t)> route_key_selector;
        {
            std::lock_guard<std::mutex> lock(strategy_mutex_);
            worker_index_selector = worker_index_selector_;
            route_key_selector    = route_key_selector_;
        }

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

	Executor Router::GetServiceExecutor(MessageType message_type, uint64 route_id) const
	{
        auto workers = route_table_.GetWorker(route_table_.ResolveServiceWorkerType(message_type));
        auto target  = PickWorker(workers, message_type, route_id).lock();
        if (!target)
            return Executor::Current();
        auto executor = target->GetExecutor();
        if (!executor)
            return Executor::Worker(target);
        return *executor;
	}
}
