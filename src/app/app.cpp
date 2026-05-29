#include "app.h"
#include "log/log_help.h"
#include "common/worker/worker_manager.h"
#include "common/worker/worker.h"
#include "common/signal/signal_handler.h"

void App::SetFrameRate(int fps)
{
	if (fps > 0)
	{
		frame_duration_ = std::chrono::milliseconds(1000 / fps);
	}
}

int App::Init()
{
	// 初始化主线程 Worker
	gb::WorkerManager::Instance()->InitMainWorker();

	// 初始化信号处理
	signal_handler_ = std::make_unique<gb::SignalHandler>();
	if (!signal_handler_->Initialize([this](gb::SignalHandler::SignalType sig_type) {
		LOG_WARN("Signal handler callback triggered");
		this->Stop();
	}))
	{
		LOG_ERROR("Failed to initialize signal handler");
		return -1;
	}

	if (OnInit() != 0)
		return -1;

	runding_ = true;
	return 0;
}

void App::Stop()
{
	runding_ = false;
	LOG_INFO("Application stop requested");
}

std::shared_ptr<gb::Worker> App::GetMainWorker() const
{
	return gb::WorkerManager::Instance()->GetMainWorker();
}

void App::Run()
{
	auto main_worker = gb::WorkerManager::Instance()->GetMainWorker();
	if (!main_worker)
	{
		LOG_ERROR("Main worker not initialized");
		return;
	}

	if (0 != OnStartup())
	{
		LOG_ERROR("OnStartup failed");
		return;
	}

	main_worker->OnStartup();
	LOG_INFO("Application started successfully");

	while (runding_)
	{
		// 检查是否收到关闭信号
		if (gb::SignalHandler::IsSignalReceived())
		{
			LOG_WARN("Signal received, initiating graceful shutdown");
			break;
		}

		auto current_time = std::chrono::steady_clock::now();

		// 调用应用层的 OnUpdate
		if (OnUpdate(0.0f) != 0)
		{
			LOG_ERROR("OnUpdate failed");
			break;
		}

		// 主线程处理帧（包括任务队列、定时器等）
		main_worker->ProcessMainFrame();

		// 调用应用层的 OnTick
		if (OnTick() != 0)
		{
			LOG_ERROR("OnTick failed");
			break;
		}

		auto frame_end_time = std::chrono::steady_clock::now();
		auto frame_time = frame_end_time - current_time;

		if (frame_time < frame_duration_)
		{
			std::this_thread::sleep_for(frame_duration_ - frame_time);
		}
	}

	LOG_INFO("Entering graceful shutdown...");

	if (0 != OnCleanup())
	{
		LOG_ERROR("OnCleanup failed");
	}

	main_worker->OnCleanup();
	LOG_INFO("Main worker cleanup completed");

	if (0 != OnUnInit())
	{
		LOG_ERROR("OnUnInit failed");
	}

	// 清理信号处理
	if (signal_handler_)
	{
		signal_handler_->Cleanup();
	}

	LOG_INFO("Application shutdown completed gracefully");
}
