#pragma once 
namespace gb
{
	struct IWorkerLogic
	{
		virtual ~IWorkerLogic() = default;
		virtual int OnStartup() = 0;
		virtual int OnUpdate(float elapsed) = 0;
		virtual int OnTick() = 0;
		virtual int OnCleanup() = 0;
	};

}