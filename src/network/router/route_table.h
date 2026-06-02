#pragma once
#include "message_type.h"
#include "worker/worker.h"
#include <array>
#include <functional>
#include <mutex>
#include <vector>
namespace gb
{

	enum MessageIntervalRange
	{
		MIR_Range = 10000
	};
	enum ServiceWorkerType : uint8_t
	{
		SWT_Normal	= 0,
		SWT_AI		= 1,
		SWT_Navigation = 2,
		SWT_Count
	};

	/// 路由表的不可变快照——冻结后由原子指针发布，读取无锁。
	struct RouteTableSnapshot
	{
		std::array<std::vector<WorkerWeakPtr>, SWT_Count> workers;
		std::function<ServiceWorkerType(MessageType)>     service_type_resolver;
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

		std::vector<WorkerWeakPtr> GetWorker(ServiceWorkerType service_worker_type) const
		{
			if (service_worker_type >= workers_.size())
			{
				return {};
			}
            std::lock_guard<std::mutex> lock(mutex_);
			return workers_[service_worker_type];
			
		}

		ServiceWorkerType ResolveServiceWorkerType(MessageType message_type) const
		{
            std::function<ServiceWorkerType(MessageType)> resolver;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                resolver = service_type_resolver_;
            }
            if (resolver)
                return resolver(message_type);
			int service_worker_type = message_type % MIR_Range;
			return (ServiceWorkerType)service_worker_type;
		}

		/// 捕获当前注册状态为快照（冻结时调用一次）。
		RouteTableSnapshot CaptureSnapshot() const
		{
            std::lock_guard<std::mutex> lock(mutex_);
			return RouteTableSnapshot{ workers_, service_type_resolver_ };
		}

		/// 不加锁地按服务类型解析（仅供持有快照的无锁读取路径使用）。
		static ServiceWorkerType ResolveServiceWorkerType(
			const RouteTableSnapshot& snapshot, MessageType message_type)
		{
			if (snapshot.service_type_resolver)
				return snapshot.service_type_resolver(message_type);
			int service_worker_type = message_type % MIR_Range;
			return (ServiceWorkerType)service_worker_type;
		}
	

private:
        mutable std::mutex                               mutex_;
        std::function<ServiceWorkerType(MessageType)>    service_type_resolver_;
        std::array<std::vector<WorkerWeakPtr>, SWT_Count> workers_;

};

}
