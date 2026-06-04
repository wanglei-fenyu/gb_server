#define CATCH_CONFIG_RUNNER

// Catch2 必须先包含以识别 CATCH_CONFIG_RUNNER
#include <catch2/catch_all.hpp>

// Catch2 定义了 CHECK 宏，与项目 log.h 中的 CHECK 冲突。
// 取消 Catch2 的 CHECK 定义，使用项目自身的版本。
// 测试代码中请使用 REQUIRE 替代。
#ifdef CHECK
#undef CHECK
#endif

#include "app_test.h"
#include <cstring>
#include <vector>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/// 将 ANSI 编码字符串（系统 ACP，中文 Windows 下为 GBK/CP936）转换为 UTF-8。
/// 子进程通过 CreateProcessW 得到的 argv 是 ANSI 编码，需要转 UTF-8
/// 才能匹配以 /utf-8 编译的测试用例名称。
static std::string AnsiToUtf8(const char* str)
{
    if (!str || !*str)
        return {};
    int wlen = MultiByteToWideChar(CP_ACP, 0, str, -1, nullptr, 0);
    std::wstring wstr(wlen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, str, -1, &wstr[0], wlen);
    int ulen = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string ustr(ulen, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &ustr[0], ulen, nullptr, nullptr);
    // Remove null terminator included by WideCharToMultiByte
    if (!ustr.empty() && ustr.back() == '\0')
        ustr.pop_back();
    return ustr;
}
#endif

TEST_CASE("smoke test", "[basic]")
{
    REQUIRE(1 + 1 == 2);
    REQUIRE(std::strlen("hello") == 5);
}

TEST_CASE("app framework initialized", "[basic]")
{
    auto worker = gb::WorkerManager::Instance()->GetWorker(1);
    REQUIRE(worker != nullptr);
    REQUIRE(worker->GetIndex() == 1);
}

static int RunCatch2(int argc, char* argv[], bool exclude_noworker)
{
    std::vector<const char*> catch_argv;
    catch_argv.reserve(argc + 3);
    catch_argv.push_back(argv[0] ? argv[0] : "unit_test");
    if (exclude_noworker)
        catch_argv.push_back("~[noworker]");
    for (int i = 1; i < argc; i++)
        catch_argv.push_back(argv[i]);
    return Catch::Session().run((int)catch_argv.size(), catch_argv.data());
}

int main(int argc, char* argv[])
{
    // UNIT_TEST_HEADLESS：子进程（fork+exec）只跑 Catch2，跳过全部 App/Worker/Lua 初始化
    if (getenv("UNIT_TEST_HEADLESS"))
    {
#ifdef _WIN32
        // Windows 子进程：argv 是系统 ANSI 编码（CP936），需转 UTF-8
        // 才能匹配 /utf-8 编译的测试用例名称
        std::vector<std::string> utf8_argv_storage;
        std::vector<char*>       utf8_argv;
        utf8_argv_storage.reserve(argc);
        utf8_argv.reserve(argc + 1);
        for (int i = 0; i < argc; i++)
        {
            utf8_argv_storage.push_back(AnsiToUtf8(argv[i]));
            utf8_argv.push_back(utf8_argv_storage.back().data());
        }
        utf8_argv.push_back(nullptr);
        return Catch::Session().run(argc, utf8_argv.data());
#else
        return RunCatch2(argc, argv, false);
#endif
    }

    auto app = std::make_unique<TestApp>(argc, argv);
    if (app->Init() != 0)
        return 1;

    // 正常模式：App/Worker 已初始化，排除 [noworker] 测试（这些测试假设无 Worker 上下文）
    int result = RunCatch2(argc, argv, true);

    // Run() 启动 Worker/Lua 并进入帧循环。
    app->Run();

    return result;
}
