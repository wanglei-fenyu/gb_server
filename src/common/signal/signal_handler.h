#pragma once
#include <functional>
#include <memory>
#include <atomic>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <signal.h>
#endif

namespace gb
{

class SignalHandler
{
public:
    enum class SignalType
    {
        SIGINT,     // Ctrl+C / SIGINT
        SIGTERM,    // SIGTERM (Linux)
    };

    using SignalCallback = std::function<void(SignalType)>;

public:
    SignalHandler();
    ~SignalHandler();

    SignalHandler(const SignalHandler&) = delete;
    SignalHandler& operator=(const SignalHandler&) = delete;

    /**
     * 初始化信号处理
     * @param callback 信号回调函数
     * @return 初始化是否成功
     */
    bool Initialize(SignalCallback callback);

    /**
     * 清理信号处理
     */
    void Cleanup();

    /**
     * 检查是否收到停止信号
     * @return true 如果收到停止信号
     */
    static bool IsSignalReceived();

private:
#ifdef _WIN32
    // Windows 平台的控制台处理程序
    static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type);
#else
    // Linux 平台的信号处理程序
    static void SignalHandlerFunc(int signal_num);
#endif

    static SignalCallback user_callback_;
    static std::atomic<bool> signal_received_;
    static std::atomic<bool> initialized_;

#ifndef _WIN32
    static void* old_sigint_handler_;
    static void* old_sigterm_handler_;
#endif
};

} // namespace gb
