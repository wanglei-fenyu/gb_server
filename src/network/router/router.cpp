#include "router.h"
#include "worker/worker_manager.h"

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
        std::lock_guard<std::shared_mutex> lock(strategy_mutex_);
        route_key_selector_ = std::move(selector);
    }

    void Router::SetWorkerIndexSelector(std::function<size_t(const std::vector<WorkerWeakPtr>&, MessageType, uint64_t)> selector)
    {
        std::lock_guard<std::shared_mutex> lock(strategy_mutex_);
        worker_index_selector_ = std::move(selector);
    }

    void Router::Freeze()
    {
        route_table_.Freeze();

        auto frozen = std::make_shared<FrozenStrategy>();
        {
            std::lock_guard<std::shared_mutex> lock(strategy_mutex_);
            frozen->route_key_selector    = route_key_selector_;
            frozen->worker_index_selector = worker_index_selector_;
        }
        frozen_strategy_.store(frozen, std::memory_order_release);
    }

    WorkerWeakPtr Router::PickWorker(const std::vector<WorkerWeakPtr>& workers, MessageType message_type, uint64 route_id) const
    {
        if (workers.empty())
            return {};

        auto frozen = frozen_strategy_.load(std::memory_order_acquire);
        if (frozen)
        {
            if (frozen->worker_index_selector)
            {
                size_t index = frozen->worker_index_selector(workers, message_type, route_id);
                if (index < workers.size())
                    return workers[index];
                return {};
            }
            uint64_t route_key = frozen->route_key_selector
                ? frozen->route_key_selector(message_type, route_id)
                : route_id;
            return workers[route_key % workers.size()];
        }

        std::function<size_t(const std::vector<WorkerWeakPtr>&, MessageType, uint64_t)> worker_index_selector;
        std::function<uint64_t(MessageType, uint64_t)> route_key_selector;
        {
            std::shared_lock<std::shared_mutex> lock(strategy_mutex_);
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

	WorkerExecutor Router::GetServiceExecutor(MessageType message_type, uint64_t route_id) const
	{
		return GetExecutor(static_cast<uint32_t>(message_type), route_id);
	}

	WorkerExecutor Router::GetExecutor(uint32_t message_type, uint64_t user_unique_id) const
	{
		if (user_unique_id == 0)
		{
			auto main_worker = WorkerManager::Instance()->GetMainWorker();
			if (main_worker)
			{
				auto executor = main_worker->GetExecutor();
				if (executor)
					return *executor;
				return WorkerExecutor::Worker(main_worker);
			}
			return {};
		}

		if (policy_ == Policy::Stateful)
		{
			uint32_t worker_index = entity_route_table_.Lookup(user_unique_id);
			if (worker_index != LockFreeRouteTable::kInvalidWorker)
			{
				auto worker = WorkerManager::Instance()->GetWorker(worker_index);
				if (worker)
				{
					auto executor = worker->GetExecutor();
					if (executor)
						return *executor;
					return WorkerExecutor::Worker(worker);
				}
				return {};
			}
			return {};
		}

		// Policy::Stateless：纯 hash 路由
		auto* workers = route_table_.GetWorkerRef(route_table_.ResolveServiceWorkerType((MessageType)message_type));
		if (!workers || workers->empty())
			return {};

		auto target = PickWorker(*workers, (MessageType)message_type, user_unique_id).lock();
		if (!target)
			return {};

		auto executor = target->GetExecutor();
		if (executor)
			return *executor;
		return WorkerExecutor::Worker(target);
	}

    // ── 实体路由 ──────────────────────────────────────────────────

    void Router::BindSingleEntity(uint64_t entity_id, uint32_t worker_index)
    {
        entity_route_table_.BindSingle(entity_id, worker_index);
    }

    void Router::BindEntity(uint64_t entity_begin, uint64_t entity_end, uint32_t worker_index)
    {
        entity_route_table_.Bind(entity_begin, entity_end, worker_index);
    }

    void Router::UnbindEntity(uint64_t entity_id)
    {
        entity_route_table_.Unbind(entity_id);
    }

    void Router::FreezeEntityRoutes()
    {
        entity_route_table_.Freeze();
    }
}
