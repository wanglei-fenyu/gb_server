#pragma once
#include "app/app.h"
#include "worker/worker_manager.h"
#include "worker/worker_logic_interface.h"
#include "log/log.h"
#include <atomic>
#include <cstdlib>
#include <string>
#include <cstdint>

class TestWorkerLogic : public gb::IWorkerLogic
{
public:
    int OnStartup() override { LOG_INFO("TestWorkerLogic::OnStartup"); return 0; }
    int OnUpdate(float) override { return 0; }
    int OnTick() override { return 0; }
    int OnCleanup() override { LOG_INFO("TestWorkerLogic::OnCleanup"); return 0; }
};

struct MsgpackTestCase
{
    const char* name;
};

class TestApp : public App
{
public:
    TestApp(int argc, char* argv[]);
    ~TestApp() override = default;

    int OnInit() override;
    int OnStartup() override;
    int OnUpdate(float) override;
    int OnTick() override;
    int OnCleanup() override;
    int OnUnInit() override { return 0; }

private:
    enum MenuLevel { MainMenu, MsgpackMenu, RouteMenu, SchedulerMenu };

    void PrintMenu();
    void HandleMenuCommand(char cmd);
    void PrintMainMenu();
    void PrintMsgpackMenu();
    void PrintRouteMenu();
    void PrintSchedulerMenu();
    void HandleMainCmd(char cmd);
    void HandleMsgpackCmd(char cmd);
    void HandleRouteCmd(char cmd);
    void HandleSchedulerCmd(char cmd);
    void RunSingleMsgpackTest(int index);
    void RunSingleRouteTest(int index);
    void RunSingleSchedulerTest(int index);
    int  RunForkedTest(const char* filter);

    // ── 调度器集成测试（需 Worker 上下文，在父进程中运行）──
    int RunWorkerExecutorDispatchTest();
    int RunWorkerExecutorMainTest();
    int RunThreadPoolDispatchTest();
    int RunThreadPoolPostTest();

    static constexpr int kMsgpackTestCount = 40;
    static const MsgpackTestCase kMsgpackTests[kMsgpackTestCount];
    static constexpr int kRouteTestCount = 24;
    static const MsgpackTestCase kRouteTests[kRouteTestCount];
    static constexpr int kSchedulerTestCount = 4;
    static const MsgpackTestCase kSchedulerTests[kSchedulerTestCount];

    std::string       exe_path_;
    MenuLevel         current_level_{MainMenu};
    std::atomic<bool> menu_quit_{false};
    std::atomic<int>  counter_{0};
    long              frame_count_{0};
    bool              menu_drawn_{false};
};
