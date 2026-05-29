#include "signal_handler.h"

namespace gb
{

std::atomic<bool> SignalHandler::signal_received_{false};
std::atomic<bool> SignalHandler::initialized_{false};
SignalHandler::SignalCallback SignalHandler::callback_{};

SignalHandler::~SignalHandler()
{
    Cleanup();
}

bool SignalHandler::Initialize(SignalCallback callback)
{
    callback_ = std::move(callback);
    signal_received_.store(false);
    if (initialized_.exchange(true))
        return true;
#ifdef _WIN32
    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE))
    {
        initialized_.store(false);
        return false;
    }
#else
    std::signal(SIGPIPE, SIG_IGN);
    if (std::signal(SIGINT, HandleSignal) == SIG_ERR)
    {
        initialized_.store(false);
        return false;
    }
    if (std::signal(SIGTERM, HandleSignal) == SIG_ERR)
    {
        initialized_.store(false);
        return false;
    }
#endif
    return true;
}

void SignalHandler::Cleanup()
{
    if (!initialized_.exchange(false))
        return;
#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
#else
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
#endif
    callback_ = {};
}

bool SignalHandler::IsSignalReceived()
{
    return signal_received_.load();
}

#ifdef _WIN32
BOOL WINAPI SignalHandler::ConsoleCtrlHandler(DWORD ctrl_type)
{
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT || ctrl_type == CTRL_CLOSE_EVENT)
    {
        signal_received_.store(true);
        if (callback_)
            callback_(SignalType::SigInt);
        return TRUE;
    }
    return FALSE;
}
#else
void SignalHandler::HandleSignal(int signal_num)
{
    signal_received_.store(true);
    if (!callback_)
        return;
    if (signal_num == SIGTERM)
        callback_(SignalType::SigTerm);
    else
        callback_(SignalType::SigInt);
}
#endif

}
