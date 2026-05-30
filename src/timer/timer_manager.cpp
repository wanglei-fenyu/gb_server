
#include "timer_manager.h"
#include "log/log.h"

namespace gb
{

void TimerManager::Update()
{    
     while (!steady_timers_.empty()) 
     {
        auto timer = steady_timers_.top();
        if (!timer->active_) {
            steady_timers_.pop();
            all_timers_.erase(timer->Id());  //鍒犻櫎
            continue;
        }

        if (timer->IsExpired()) {
            steady_timers_.pop();
            (*timer)();
            if (timer->IsLoop()) {
                timer->Reset();
                steady_timers_.push(timer); // 閲嶆柊鍔犲叆寰幆瀹氭椂鍣?
            } else {
                all_timers_.erase(timer->Id()); // 浠庡叏灞€绠＄悊涓垹闄?
            }
        } 
        else {
            break;
        }
     }

     while (!system_timers_.empty()) 
     {
        auto timer = system_timers_.top();
        if (!timer->active_) {
            system_timers_.pop();
            all_timers_.erase(timer->Id());
            continue;
        }

        if (timer->IsExpired()) {
            system_timers_.pop();
            (*timer)();
            if (timer->IsLoop()) {
                timer->Reset();
                system_timers_.push(timer); // 閲嶆柊鍔犲叆寰幆瀹氭椂鍣?
            } else {
                all_timers_.erase(timer->Id()); // 浠庡叏灞€绠＄悊涓垹闄?
            }
        } 
        else {
            break;
        }
     }
}

int64_t TimerManager::RegisterTimer(int64_t milliseconds, std::function<void()>&& callFunc, bool loop /*= false*/)
{
    return RegisterTimer(std::chrono::milliseconds(milliseconds), std::move(callFunc), loop);
}

int64_t TimerManager::RegisterTimer(std::chrono::milliseconds time, std::function<void()>&& callFunc, bool loop /*= false*/)
{
    // 鍏抽棴鏈熼棿鎷掔粷鏂扮殑瀹氭椂鍣ㄦ敞鍐?
    if (shutting_down_.load())
    {
        return -1;
    }
    
	auto id = ++generate_timer_id_;
	auto timer = std::make_unique<SteadyTimer>(time, id, loop, std::move(callFunc));
	steady_timers_.push(timer.get());
	all_timers_.emplace(id, std::move(timer));
	return id;
}

int64_t TimerManager::RegisterSystemTimer(int64_t milliseconds, std::function<void()>&& callFunc, bool loop /*= false*/)
{
	return RegisterSystemTimer(std::chrono::milliseconds(milliseconds), std::move(callFunc), loop);
}

int64_t TimerManager::RegisterSystemTimer(std::chrono::milliseconds time, std::function<void()>&& callFunc, bool loop /*= false*/)
{
    // 鍏抽棴鏈熼棿鎷掔粷鏂扮殑瀹氭椂鍣ㄦ敞鍐?
    if (shutting_down_.load())
    {
        return -1;
    }
    
	auto id = ++generate_timer_id_;
	auto timer = std::make_unique<SystemTimer>(time, id, loop, std::move(callFunc));
	system_timers_.push(timer.get());
	all_timers_.emplace(id, std::move(timer));
	return id;
}

void TimerManager::UnRegisterTimer(int64_t timerId)
{
	 auto it = all_timers_.find(timerId);
        if (it != all_timers_.end()) {
            it->second->Cancel(); //鍏堟爣璁颁负澶辨晥
            //all_timers_.erase(it);
        }
}

Timer* TimerManager::GetTimer(int64_t timerId)
{
	 auto it = all_timers_.find(timerId);
      return (it != all_timers_.end()) ? it->second.get() : nullptr;
}

void TimerManager::EnterShutdownMode()
{
    shutting_down_.store(true);
    LOG_INFO("TimerManager entering shutdown mode - completing current frame timers, cancelling future ones");
    
    // 鍙栨秷鎵€鏈夋湭杩囨湡鐨勫畾鏃跺櫒浠ラ槻姝㈠畠浠璋冨害
    for (auto& [id, timer] : all_timers_)
    {
        if (!timer->IsExpired() && timer->active_)
        {
            timer->Cancel();
        }
    }
}

}
