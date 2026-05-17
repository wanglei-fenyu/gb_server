#pragma once

#include "common\worker\worker.h"
#include "route_table.h"
#include "message_type.h"
namespace gb
{

	struct Router
	{
		
		void RegisterWorker(ServiceWorkerType service_worker_type, WorkerWeakPtr worker);
		
		WorkerWeakPtr GetServiceWorker(MessageType message_type, uint64 player_id);

	private:
		RouteTable route_table_;
	};



}


