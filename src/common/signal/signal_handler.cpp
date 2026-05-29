#include "signal_handler.h"
#include "../log/log_help.h"

namespace gb
{

SignalHandler::SignalCallback SignalHandler::user_callback_ = nullptr;
std::atomic<bool> SignalHandler::signal_received_(false);
std::atomic<bool> SignalHandler::initialized_(false);

#ifndef _WIN32
void* SignalHandler::old_sigint_handler_ = nullptr;
void* SignalHandler::old_sigterm_handler_ = nullptr;
#endif

SignalHandler::SignalHandler()
{
}

SignalHandler::~SignalHandler()
{
    Cleanup();
}

bool SignalHandler::Initialize(SignalCallback callback)
{
    if (initialized_.exchange(true))
    {
        LOG_WARN("SignalHandler already initialized");
        return true;
    }

    user_callback_ = callback;
    signal_received_.store(false);

#ifdef _WIN32
    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE))
    {
        LOG_ERROR("Failed to register console control handler");
        initialized_.store(false);
        return false;
    }
    LOG_INFO("Signal handler initialized for Windows (Ctrl+C)");
#else
    signal(SIGPIPE, SIG_IGN);

    old_sigint_handler_ = signal(SIGINT, SignalHandlerFunc);
    if (old_sigint_handler_ == SIG_ERR)
    {
        LOG_ERROR("Failed to register SIGINT handler");
        initialized_.store(false);
        return false;
    }

    old_sigterm_handler_ = signal(SIGTERM, SignalHandlerFunc);
    if (old_sigterm_handler_ == SIG_ERR)
    {
        LOG_ERROR("Failed to register SIGTERM handler");
        signal(SIGINT, (void (*)(int))old_sigint_handler_);
        initialized_.store(false);
        return false;
    }

    LOG_INFO("Signal handler initialized for Linux (SIGINT/SIGTERM)");
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
    if (old_sigint_handler_ != nullptr)
    {
        signal(SIGINT, (void (*)(int))old_sigint_handler_);
        old_sigint_handler_ = nullptr;
    }
    if (old_sigterm_handler_ != nullptr)
    {
        signal(SIGTERM, (void (*)(int))old_sigterm_handler_);
        old_sigterm_handler_ = nullptr;
    }
#endif

    user_callback_ = nullptr;
    LOG_INFO("Signal handler cleaned up");
}

bool SignalHandler::IsSignalReceived()
{
    return signal_received_.load();
}

#ifdef _WIN32
BOOL WINAPI SignalHandler::ConsoleCtrlHandler(DWORD ctrl_type)
{
    switch (ctrl_type)
    {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        {
            LOG_WARN("Received Ctrl+C event, initiating graceful shutdown...");
            signal_received_.store(true);
            if (user_callback_)
            {
                user_callback_(SignalType::SIGINT);
            }
            return TRUE;
        }
        case CTRL_CLOSE_EVENT:
        {
            LOG_WARN("Console window is closing, initiating graceful shutdown...");
            signal_received_.store(true);
            if (user_callback_)
            {
                user_callback_(SignalType::SIGINT);
            }
            return TRUE;
        }
        default:
            return FALSE;
    }
}

#else
void SignalHandler::SignalHandlerFunc(int signal_num)
{
    SignalType sig_type = SignalType::SIGINT;

    switch (signal_num)
    {
        case SIGINT:
        {
            LOG_WARN("Received SIGINT (Ctrl+C), initiating graceful shutdown...");
            sig_type = SignalType::SIGINT;
            break;
        }
        case SIGTERM:
        {
            LOG_WARN("Received SIGTERM, initiating graceful shutdown...");
            sig_type = SignalType::SIGTERM;
            break;
        }
        default:
            return;
    }

    signal_received_.store(true);
    if (user_callback_)
    {
        user_callback_(sig_type);
    }
}
#endif

} // namespace gb
