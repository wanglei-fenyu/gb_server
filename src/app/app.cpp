#include "app.h"
#include "log/log_help.h"
#include "common/worker/worker_manager.h"
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
    gb::WorkerManager::Instance()->InitMainWorker();
    signal_handler_ = std::make_unique<gb::SignalHandler>();
    if (!signal_handler_->Initialize([this](gb::SignalHandler::SignalType) { Stop(); }))
        return -1;
    if (OnInit() != 0) return -1;
    runding_ = true;
    return 0;
}

void App::Stop()
{
    runding_ = false;
}

void App::Run()
{
   auto main_worker = gb::WorkerManager::Instance()->GetMainWorker();
   if (!main_worker)
       return;

   if (0 != OnStartup())
   {
	   return;
   }

   main_worker->OnStartup();

    auto last_time = std::chrono::steady_clock::now();
    while (runding_)
    {
        if (gb::SignalHandler::IsSignalReceived())
            break;
        auto                         current_time = std::chrono::steady_clock::now();
        std::chrono::duration<float> elapsed      = current_time - last_time;
        last_time                                 = current_time;
        if (OnUpdate(elapsed.count()) != 0)
        {
            break;
        }

        main_worker->ProcessFrame(elapsed.count());

	    if (OnTick() != 0)
	    {
		   break;
	    }

        auto frame_end_time = std::chrono::steady_clock::now();
        auto frame_time     = frame_end_time - current_time;

        if (frame_time < frame_duration_)
        {
            std::this_thread::sleep_for(frame_duration_ - frame_time);
        }
    }

	if (0 != OnCleanup())
	{
		return;
	}

    main_worker->OnCleanup();

    OnUnInit();

    if (signal_handler_)
        signal_handler_->Cleanup();
}
