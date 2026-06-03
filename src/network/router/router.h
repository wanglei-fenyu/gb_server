#pragma once

#include "worker/worker.h"
#include "route_table.h"
#include "message_type.h"
#include "lock_free_route_table.h"
#include "network/rpc/executor.h"
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
		
		WorkerExecutor GetServiceExecutor(MessageType message_type, uint64 route_id) const;

        // ── 实体路由（Phase 2） ──────────────────────────────────

        /// 通过 entity_id 查找所属 Worker，返回 WorkerExecutor。
        /// 若未找到则返回无效的 WorkerExecutor（HasWorker() == false）。
        WorkerExecutor GetEntityExecutor(uint64_t entity_id) const;

        /// 绑定实体区间到指定 Worker（主线程调用）。
        void BindEntity(uint64_t entity_begin, uint64_t entity_end, uint32_t worker_index);

        /// 解绑单例实体（主线程调用）。
        void UnbindEntity(uint64_t entity_id);

        /// 冻结实体路由表（主线程每帧调用，与 App::ProcessMainThreadEvents 配合）。
        void FreezeEntityRoutes();

        /// 获取实体路由表引用（供外部调试/监控）。
        const LockFreeRouteTable& GetEntityRouteTable() const { return entity_route_table_; }

	private:
        WorkerWeakPtr PickWorker(const std::vector<WorkerWeakPtr>& workers, MessageType message_type, uint64 route_id) const;

    private:
		RouteTable route_table_;
        mutable std::mutex                               strategy_mutex_;
        std::function<uint64_t(MessageType, uint64_t)>  route_key_selector_;
        std::function<size_t(const std::vector<WorkerWeakPtr>&, MessageType, uint64_t)> worker_index_selector_;

        /// 无锁实体路由表（entity_id → WorkerIndex）
        LockFreeRouteTable entity_route_table_;
	};



}

