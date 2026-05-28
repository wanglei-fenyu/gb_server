#pragma once
#include "message_type.h"
#include "log/log_help.h"
#include "common/worker/worker.h"
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


	struct RouteTable
	{
        void RegisterWorker(ServiceWorkerType service_worker_type, WorkerWeakPtr worker)
		{
			if (service_worker_type < 0 || service_worker_type >= workers_.size())
			{
				return;
			}

			workers_[service_worker_type].push_back(worker);
        }


		std::vector<WorkerWeakPtr>& GetWorker(ServiceWorkerType service_worker_type)
		{
		
			static std::vector<WorkerWeakPtr> null;
			if (service_worker_type < 0 || service_worker_type >= workers_.size())
			{
				return null;
			}

			return workers_[service_worker_type];
			
		}

		std::vector<WorkerWeakPtr>& GetWorkerByMessageType(MessageType message_type)
		{
			int service_worker_type = message_type % MIR_Range;
			return GetWorker((ServiceWorkerType)service_worker_type);
		}
	

private:
        std::array<std::vector<WorkerWeakPtr>, SWT_Count> workers_;

};

}
