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

int main(int argc, char* argv[])
{
    // UNIT_TEST_HEADLESS：子进程（fork+exec）只跑 Catch2，跳过全部 App/Worker/Lua 初始化
    if (getenv("UNIT_TEST_HEADLESS"))
        return Catch::Session().run(argc, argv);

    auto app = std::make_unique<TestApp>(argc, argv);
    if (app->Init() != 0)
        return 1;

    int result = Catch::Session().run(argc, argv);

    // Run() 启动 Worker/Lua 并进入帧循环。
    app->Run();

    return result;
}
