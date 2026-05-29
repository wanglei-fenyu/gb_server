#pragma once
#include <functional>
#include <atomic>

#ifdef _WIN32
#include <winsock2.h> //windows的坑 ，必须在windows.h之前包含winsock2.h，否则会引发编译错误
#include <windows.h>
#else
#include <csignal>
#endif

namespace gb
{

class SignalHandler
{
public:
    enum class SignalType
    {
        SigInt,
        SigTerm,
    };

    using SignalCallback = std::function<void(SignalType)>;

public:
    SignalHandler() = default;
    ~SignalHandler();

    bool Initialize(SignalCallback callback);
    void Cleanup();

    static bool IsSignalReceived();

private:
#ifdef _WIN32
    static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type);
#else
    static void HandleSignal(int signal_num);
#endif

private:
    static std::atomic<bool> signal_received_;
    static std::atomic<bool> initialized_;
    static SignalCallback callback_;
};

}
