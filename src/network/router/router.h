#pragma once

#include "common/worker/worker.h"
#include "route_table.h"
#include "message_type.h"
#include "network/scheduler/executor.h"
#include <functional>
#include <mutex>
namespace gb
{

	struct Router
	{
		
		void RegisterWorker(ServiceWorkerType service_worker_type, WorkerWeakPtr worker);
        void SetServiceTypeResolver(std::function<ServiceWorkerType(MessageType)> resolver);
        void SetRouteKeySelector(std::function<uint64_t(MessageType, uint64_t)> selector);
        void SetWorkerIndexSelector(std::function<size_t(const std::vector<WorkerWeakPtr>&, MessageType, uint64_t)> selector);
		
		Executor GetServiceExecutor(MessageType message_type, uint64 route_id) const;

	private:
        WorkerWeakPtr PickWorker(const std::vector<WorkerWeakPtr>& workers, MessageType message_type, uint64 route_id) const;

    private:
		RouteTable route_table_;
        mutable std::mutex                              strategy_mutex_;
        std::function<uint64_t(MessageType, uint64_t)> route_key_selector_;
        std::function<size_t(const std::vector<WorkerWeakPtr>&, MessageType, uint64_t)> worker_index_selector_;
	};



}
