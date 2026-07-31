#pragma once
#include "message_type.h"
#include "service_worker_type.h"
#include "worker/worker.h"
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
namespace gb
{

	enum MessageIntervalRange
	{
		MIR_Range = 10000
	};


	struct RouteTable
	{
        void RegisterWorker(ServiceWorkerType service_worker_type, WorkerWeakPtr worker)
		{
			if (service_worker_type >= workers_.size())
			{
				return;
			}
            std::lock_guard<std::mutex> lock(mutex_);
			workers_[service_worker_type].push_back(worker);
        }


		void SetServiceTypeResolver(std::function<ServiceWorkerType(MessageType)> resolver)
		{
            std::lock_guard<std::mutex> lock(mutex_);
            service_type_resolver_ = std::move(resolver);
		}

		/// 将所有可变数据快照为不可变原子指针 —— 之后 GetWorker / ResolveServiceWorkerType 零锁零拷贝。
		/// 必须在所有 RegisterWorker / SetServiceTypeResolver 之后、任何消息分发之前调用。
		/// 冻结前的读取仍然可用（走旧的 mutex 路径）。
		void Freeze()
		{
			auto snapshot = std::make_shared<FrozenSnapshot>();
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if (service_type_resolver_)
					snapshot->resolver = service_type_resolver_;
				for (size_t i = 0; i < workers_.size(); ++i)
				{
					if (!workers_[i].empty())
						snapshot->workers[i] = std::make_shared<const std::vector<WorkerWeakPtr>>(workers_[i]);
				}
			}
			frozen_.store(snapshot, std::memory_order_release);
		}

		bool IsFrozen() const
		{
			return frozen_.load(std::memory_order_acquire) != nullptr;
		}

		std::vector<WorkerWeakPtr> GetWorker(ServiceWorkerType service_worker_type) const
		{
			auto frozen = frozen_.load(std::memory_order_acquire);
			if (frozen && service_worker_type < SWT_Count)
			{
				auto& ptr = frozen->workers[service_worker_type];
				if (ptr) return *ptr;
				return {};
			}
			if (service_worker_type >= workers_.size())
			{
				return {};
			}
            std::lock_guard<std::mutex> lock(mutex_);
			return workers_[service_worker_type];
		}

		/// 零拷贝版本 —— 仅在冻结后可用。返回内部指针，无任何分配。
		const std::vector<WorkerWeakPtr>* GetWorkerRef(ServiceWorkerType service_worker_type) const
		{
			auto frozen = frozen_.load(std::memory_order_acquire);
			if (frozen && service_worker_type < SWT_Count)
				return frozen->workers[service_worker_type].get();
			return nullptr;
		}

		ServiceWorkerType ResolveServiceWorkerType(MessageType message_type) const
		{
			auto frozen = frozen_.load(std::memory_order_acquire);
			if (frozen && frozen->resolver)
				return frozen->resolver(message_type);
            std::function<ServiceWorkerType(MessageType)> resolver;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                resolver = service_type_resolver_;
            }
            if (resolver)
                return resolver(message_type);
			// 不再有默认的 % 10000 映射 —— 所有未显式设置 resolver 的消息统一走 Normal Worker。
			// 需要 AI/Navigation 路由的服务必须通过 SetServiceTypeResolver 设置显式映射。
			return SWT_Normal;
		}

	private:
		/// 不可变快照 —— Freeze() 后通过 atomic 指针发布，生命周期为进程级别（永不释放）
		struct FrozenSnapshot
		{
			std::function<ServiceWorkerType(MessageType)> resolver;
			std::array<std::shared_ptr<const std::vector<WorkerWeakPtr>>, SWT_Count> workers;
		};

	private:
        mutable std::mutex                               mutex_;
        std::function<ServiceWorkerType(MessageType)>    service_type_resolver_;
        std::array<std::vector<WorkerWeakPtr>, SWT_Count> workers_;

		/// 冻结后的只读快照（冻结前为 nullptr）
		std::atomic<std::shared_ptr<const FrozenSnapshot>> frozen_{nullptr};
	};

}
