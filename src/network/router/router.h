#pragma once

#include "worker/worker.h"
#include "route_table.h"
#include "message_type.h"
#include "network/rpc/executor.h"
#include <atomic>
#include <functional>
#include <mutex>
namespace gb
{

	struct Router
	{
		~Router();

		void RegisterWorker(ServiceWorkerType service_worker_type, WorkerWeakPtr worker);
        void SetServiceTypeResolver(std::function<ServiceWorkerType(MessageType)> resolver);
        void SetRouteKeySelector(std::function<uint64_t(MessageType, uint64_t)> selector);
        void SetWorkerIndexSelector(std::function<size_t(const std::vector<WorkerWeakPtr>&, MessageType, uint64_t)> selector);

        /// 冻结路由表与策略为只读快照；此后 GetServiceExecutor 走无锁路径。
        /// 必须在所有 RegisterWorker / SetXxx 调用之后、任何消息分发之前调用一次。
        void Freeze();
		
		WorkerExecutor GetServiceExecutor(MessageType message_type, uint64 route_id) const;

	private:
        WorkerWeakPtr PickWorker(
            const std::vector<WorkerWeakPtr>& workers,
            const std::function<size_t(const std::vector<WorkerWeakPtr>&, MessageType, uint64_t)>& worker_index_selector,
            const std::function<uint64_t(MessageType, uint64_t)>& route_key_selector,
            MessageType message_type, uint64 route_id) const;

        WorkerExecutor ToExecutor(WorkerWeakPtr target) const;

    private:
        struct Snapshot
        {
            RouteTableSnapshot route_table;
            std::function<uint64_t(MessageType, uint64_t)>  route_key_selector;
            std::function<size_t(const std::vector<WorkerWeakPtr>&, MessageType, uint64_t)> worker_index_selector;
        };

		RouteTable route_table_;
        mutable std::mutex                               strategy_mutex_;
        std::function<uint64_t(MessageType, uint64_t)>  route_key_selector_;
        std::function<size_t(const std::vector<WorkerWeakPtr>&, MessageType, uint64_t)> worker_index_selector_;

        /// 冻结快照——Freeze() 时分配一次，进程生命周期内不释放（析构时回收）。
        std::atomic<const Snapshot*> frozen_{nullptr};
	};



}
