#pragma once
#include "app_def.h"
#include <memory>
#include <chrono>
#include <atomic>
#include "log/log_help.h"

namespace gb
{
class Worker;
class SignalHandler;
}

class App
{
public:
	App(int argc, char* argv[])
		: runding_(false), frame_duration_(std::chrono::milliseconds(16)) {}
	virtual ~App() {}

	APP_TYPE GetAppType() { return appType_; }
	void SetFrameRate(int fps);
	int Init();
	void Stop();
	void Run();

	// 获取主线程 Worker（用于业务代码投递任务）
	std::shared_ptr<gb::Worker> GetMainWorker() const;

protected:
	virtual int OnInit() = 0;
	virtual int OnStartup() = 0;
	virtual int OnUpdate(float) = 0;
	virtual int OnTick() = 0;
	virtual int OnCleanup() = 0;
	virtual int OnUnInit() = 0;

protected:
	APP_TYPE appType_;

private:
	std::atomic<bool> runding_;
	std::chrono::milliseconds frame_duration_;
	std::unique_ptr<gb::SignalHandler> signal_handler_;
};
