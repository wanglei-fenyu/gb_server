#include "router.h"

namespace gb
{

	void Router::RegisterWorker(ServiceWorkerType service_worker_type, WorkerWeakPtr worker)
	{
		route_table_.RegisterWorker(service_worker_type, worker);
	}

	WorkerWeakPtr Router::GetServiceWorker(MessageType message_type, uint64 player_id)
	{
        std::vector<WorkerWeakPtr>& workers = route_table_.GetWorkerByMessageType(message_type);
        if (workers.empty())
            return {};

		int index = player_id % workers.size();
        return workers[index];
	}
}
