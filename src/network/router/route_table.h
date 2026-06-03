#pragma once
#include "message_type.h"
#include "service_worker_type.h"
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
			// 不再有默认的 % 10000 映射 —— 所有未显式设置 resolver 的消息统一走 Normal Worker。
			// 需要 AI/Navigation 路由的服务必须通过 SetServiceTypeResolver 设置显式映射。
			return SWT_Normal;
		}
	

private:
        mutable std::mutex                               mutex_;
        std::function<ServiceWorkerType(MessageType)>    service_type_resolver_;
        std::array<std::vector<WorkerWeakPtr>, SWT_Count> workers_;

};

}
