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

	class Router
	{
	public:
		enum class Policy : uint8_t {
			Stateful,   // entity_id 精确绑定：Scene Server 等有状态服务器
			Stateless,  // hash 路由：Gateway Server 等无状态服务器
		};

		/// 设置路由策略（必须在所有 RegisterWorker 之前调用）
		void SetPolicy(Policy p) { policy_ = p; }
		Policy GetPolicy() const { return policy_; }

		void RegisterWorker(ServiceWorkerType service_worker_type, WorkerWeakPtr worker);
		void SetServiceTypeResolver(std::function<ServiceWorkerType(MessageType)> resolver);
		void SetRouteKeySelector(std::function<uint64_t(MessageType, uint64_t)> selector);
		void SetWorkerIndexSelector(std::function<size_t(const std::vector<WorkerWeakPtr>&, MessageType, uint64_t)> selector);

		/// 统一路由入口：
		///   entity_id == 0：系统消息（etcd 等）→ main_worker_（主线程）
		///   entity_id != 0：
		///     - Policy::Stateful：查 entity_route_table_，未命中 → 丢弃
		///     - Policy::Stateless：纯 hash 路由
		///   路由失败 → 返回空 executor，消息丢弃
		WorkerExecutor GetExecutor(uint32_t message_type, uint64_t entity_id) const;

		/// 兼容旧接口，内部转发到 GetExecutor
		WorkerExecutor GetServiceExecutor(MessageType message_type, uint64_t route_id) const;

		// ── 实体路由（Stateful 策略专用） ──────────────────────

		/// 绑定区间到指定 Worker（主线程调用）。
		void BindEntity(uint64_t entity_begin, uint64_t entity_end, uint32_t worker_index);

		/// 解绑单例实体（主线程调用）。
		void UnbindEntity(uint64_t entity_id);

		/// 冻结实体路由表（主线程每帧调用）。
		void FreezeEntityRoutes();

		/// 获取实体路由表引用（供外部调试/监控）。
		const LockFreeRouteTable& GetEntityRouteTable() const { return entity_route_table_; }

	private:
		WorkerWeakPtr PickWorker(const std::vector<WorkerWeakPtr>& workers, MessageType message_type, uint64_t route_id) const;

	private:
		Policy policy_ = Policy::Stateless;

		RouteTable route_table_;
		mutable std::mutex                               strategy_mutex_;
		std::function<uint64_t(MessageType, uint64_t)>  route_key_selector_;
		std::function<size_t(const std::vector<WorkerWeakPtr>&, MessageType, uint64_t)> worker_index_selector_;

		/// 无锁实体路由表（entity_id → WorkerIndex）
		LockFreeRouteTable entity_route_table_;
	};

}

