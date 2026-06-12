#include "app_test.h"
#include "base/res_path.h"
#include "async/thread_pool_scheduler.h"
#include "network/rpc/executor.h"
#include "msgpack/msgpack.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <condition_variable>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <conio.h>
#else
#include <poll.h>
#include <unistd.h>
#include <sys/wait.h>
#endif

const MsgpackTestCase TestApp::kMsgpackTests[] = {
    {"msgpack: int8 序列化反序列化"},
    {"msgpack: int16 序列化反序列化"},
    {"msgpack: int32 序列化反序列化"},
    {"msgpack: int64 序列化反序列化"},
    {"msgpack: uint8 序列化反序列化"},
    {"msgpack: uint16 序列化反序列化"},
    {"msgpack: uint32 序列化反序列化"},
    {"msgpack: uint64 序列化反序列化"},
    {"msgpack: float 序列化反序列化"},
    {"msgpack: double 序列化反序列化"},
    {"msgpack: bool 序列化反序列化"},
    {"msgpack: string 序列化反序列化"},
    {"msgpack: const char* 打包为 string"},
    {"msgpack: 枚举序列化反序列化"},
    {"msgpack: nullptr 空值序列化反序列化"},
    {"msgpack: 多值打包/解包"},
    {"msgpack: 通过 Packer/Unpacker 多值操作"},
    {"msgpack: std::array 序列化反序列化"},
    {"msgpack: vector<int> 序列化反序列化"},
    {"msgpack: 空 vector 序列化反序列化"},
    {"msgpack: vector<string> 序列化反序列化"},
    {"msgpack: vector<float> 序列化反序列化"},
    {"msgpack: list<int> 序列化反序列化"},
    {"msgpack: map<string,int> 序列化反序列化"},
    {"msgpack: 空 map 序列化反序列化"},
    {"msgpack: unordered_map<int,int> 序列化反序列化"},
    {"msgpack: vector<uint8_t> 二进制序列化反序列化"},
    {"msgpack: 空二进制序列化反序列化"},
    {"msgpack: 时间点序列化反序列化"},
    {"msgpack: REGISTER_PACKER 自定义对象序列化反序列化"},
    {"msgpack: REGISTER_PACKER 默认值对象序列化反序列化"},
    {"msgpack: Packer vector/move/clear 接口"},
    {"msgpack: Unpacker empty/set_data 接口"},
    {"msgpack: 自由函数 unpack 带错误码"},
    {"msgpack: 解包空数据产生错误"},
    {"msgpack: 解包截断数据产生错误"},
    {"msgpack: 小整数使用更紧凑的编码格式"},
    {"msgpack: 整数值的 float/double 编码为整数类型"},
    {"msgpack: 单值 tuple 序列化反序列化"},
    {"msgpack: 异构 tuple 自由函数序列化反序列化"},
};

const MsgpackTestCase TestApp::kSchedulerTests[] = {
    {"WorkerExecutor 绑定 Worker(1) — Dispatch 投递"},
    {"WorkerExecutor::Current() — 主线程分发"},
    {"ThreadPoolScheduler Dispatch<int> — Worker 上下文"},
    {"ThreadPoolScheduler Post — Worker 上下文"},
};

const MsgpackTestCase TestApp::kRouteTests[] = {
    {"route: 空表 Lookup 返回 kInvalidWorker"},
    {"route: Bind 单例并能 Lookup"},
    {"route: Bind 区间并在区间内查找"},
    {"route: Bind 区间区间外返回 Invalid"},
    {"route: 重叠 Bind 覆盖旧条目"},
    {"route: Unbind 移除单例"},
    {"route: Unbind 从区间中间拆分"},
    {"route: Unbind 区间开头"},
    {"route: Unbind 区间末尾"},
    {"route: Freeze 发布多条目变更"},
    {"route: 默认 resolver 返回 SWT_Normal"},
    {"route: 自定义 resolver 映射消息类型"},
    {"route: RegisterWorker + GetWorker 一对一"},
    {"route: RegisterWorker 多 worker 注册"},
    {"route: Router::GetEntityExecutor 未绑定丢弃（Stateful）"},
    {"route: Router::GetServiceExecutor user_unique_id==0 路由到 main_worker_"},
    {"route: Router::GetExecutor(Stateful) user_unique_id==0 路由到 main_worker_"},
    {"route: Router::Bind→Freeze→GetExecutor(Stateful) 验证路由表"},
    {"route: SequenceId 编解码往返"},
    {"route: SequenceId 零值"},
    {"route: SequenceId 边界值"},
    {"route: SequenceId worker_index 编码偏移"},
    {"route: Meta 默认构造各字段为零"},
    {"route: MsgMode 枚举值定义"},
};

TestApp::TestApp(int argc, char* argv[])
    : App(argc, argv)
    , exe_path_(argv[0])
{
    ResPath::Instance()->SetResRootPath("./res");
}

int TestApp::OnInit()
{
    auto worker = gb::WorkerManager::Instance()->CreateWorker(
        std::make_shared<TestWorkerLogic>(), gb::SWT_Normal);
    if (!worker)
    {
        LOG_ERROR("Failed to create worker");
        return -1;
    }
    LOG_INFO("TestApp::OnInit - worker created");
    return 0;
}

int TestApp::OnStartup()
{
    auto workers = gb::WorkerManager::Instance()->GetWorkers();
    for (auto& w : workers)
    {
        if (w)
            w->OnStartup();
    }
    return 0;
}

// ─── Main Menu ──────────────────────────────────────────────

void TestApp::PrintMainMenu()
{
    printf("\n======= 主菜单 =======\n");
    printf("  [1] Say hello        -- 发送日志任务\n");
    printf("  [2] 计算 1+2+...+100  -- 发送累加任务\n");
    printf("  [3] 递增计数器        -- 发送计数任务\n");
    printf("  [4] 查看状态          -- 帧数 / 计数 / 线程池\n");
    printf("  [5] msgpack 测试      -- 进入 msgpack 二级菜单\n");
    printf("  [6] 路由测试           -- 进入路由二级菜单（实体路由/服务路由/RPC 序列号）\n");
    printf("  [7] msgpack 打解包演示 -- 打包/解包并显示二进制格式\n");
    printf("  [8] 调度器测试       -- 进入调度器测试菜单\n");
    printf("  [q] 退出              -- 退出程序\n");
    printf("========================\n");
    printf("> ");
    fflush(stdout);
}

void TestApp::HandleMainCmd(char cmd)
{
    auto worker = gb::WorkerManager::Instance()->GetWorker(1);
    if (!worker)
    {
        printf("  [错误] Worker 不可用\n");
        return;
    }

    switch (cmd)
    {
    case '1':
        worker->Post([]() {
            LOG_INFO("[Worker Task] Hello from worker thread!");
        });
        printf("  -> 已投递日志任务\n");
        break;

    case '2':
        worker->Post([]() {
            int sum = 0;
            for (int i = 1; i <= 100; i++)
                sum += i;
            LOG_INFO("[Worker Task] 1+2+...+100 = {}", sum);
        });
        printf("  -> 已投递累加任务\n");
        break;

    case '3':
        worker->Post([this]() {
            int val = ++counter_;
            LOG_INFO("[Worker Task] Counter incremented to {}", val);
        });
        printf("  -> 已投递计数任务\n");
        break;

    case '4': {
        auto tp     = gb::ThreadPoolScheduler::Instance();
        size_t pending = tp ? tp->PendingCount() : 0;
        printf("  frames=%ld  counter=%d  threadpool_pending=%zu  workers=%zu\n",
               frame_count_, counter_.load(), pending,
               gb::WorkerManager::Instance()->Size());
        break;
    }

    case '5':
        current_level_ = MsgpackMenu;
        break;

    case '6':
        current_level_ = RouteMenu;
        break;

    case '8':
        current_level_ = SchedulerMenu;
        break;

    case '7': {
        gb::msgpack::Packer packer;
        packer(int32_t{42}, 3.14f, std::string{"hello"}, true);
        const auto& data = packer.vector();
        printf("  -> bytes(%zu) = ", data.size());
        for (auto b : data)
            printf("%02x ", b);
        printf("\n");
        std::error_code ec;
        auto [i, f, s, b] = gb::msgpack::unpack<int32_t, float, std::string, bool>(data, ec);
        if (ec)
            printf("  -> unpack error: %s\n", ec.message().c_str());
        else
            printf("  -> unpack(int32:%d, float:%g, str:%s, bool:%s)\n",
                   i, f, s.c_str(), b ? "true" : "false");
        fflush(stdout);
        break;
    }

    case 'q':
        menu_quit_ = true;
        printf("  正在退出...\n");
        return;

    default:
        printf("  未知命令: '%c'\n", cmd);
        break;
    }

    menu_drawn_ = false;
}

// ─── Msgpack Submenu ────────────────────────────────────────

void TestApp::PrintMsgpackMenu()
{
    printf("\n======= msgpack 测试用例 (%d) =======\n", kMsgpackTestCount);
    for (int i = 0; i < kMsgpackTestCount; i++)
    {
        printf("  [%2d] %s\n", i + 1, kMsgpackTests[i].name);
    }
    printf("  [a] 运行全部\n");
    printf("  [b] 返回主菜单\n");
    printf("==================================\n");
    printf("> ");
    fflush(stdout);
}

void TestApp::HandleMsgpackCmd(char cmd)
{
    if (cmd == 'b' || cmd == 'B')
    {
        current_level_ = MainMenu;
        menu_drawn_ = false;
        return;
    }

    if (cmd == 'a' || cmd == 'A')
    {
        printf("  -> 运行全部 msgpack 测试...\n");
        fflush(stdout);
        int ret = RunForkedTest("[msgpack]");
        if (ret == 0)
            printf("  -> 全部 msgpack 测试通过\n");
        else
            printf("  -> msgpack 测试失败 (exit=%d)\n", ret);
        fflush(stdout);
        menu_drawn_ = false;
        return;
    }

    printf("  未知命令: '%c'\n", cmd);
    menu_drawn_ = false;
}

// ─── Route Submenu ────────────────────────────────────────

void TestApp::PrintRouteMenu()
{
    printf("\n======= 路由测试用例 (%d) =======\n", kRouteTestCount);
    printf("  ── 实体路由 ──\n");
    printf("  [ 1] %s\n", kRouteTests[0].name);
    printf("  [ 2] %s\n", kRouteTests[1].name);
    printf("  [ 3] %s\n", kRouteTests[2].name);
    printf("  [ 4] %s\n", kRouteTests[3].name);
    printf("  [ 5] %s\n", kRouteTests[4].name);
    printf("  [ 6] %s\n", kRouteTests[5].name);
    printf("  [ 7] %s\n", kRouteTests[6].name);
    printf("  [ 8] %s\n", kRouteTests[7].name);
    printf("  [ 9] %s\n", kRouteTests[8].name);
    printf("  [10] %s\n", kRouteTests[9].name);
    printf("  ── 服务路由 ──\n");
    printf("  [11] %s\n", kRouteTests[10].name);
    printf("  [12] %s\n", kRouteTests[11].name);
    printf("  [13] %s\n", kRouteTests[12].name);
    printf("  [14] %s\n", kRouteTests[13].name);
    printf("  ── Router 调度 ──\n");
    printf("  [15] %s\n", kRouteTests[14].name);
    printf("  [16] %s\n", kRouteTests[15].name);
    printf("  [17] %s\n", kRouteTests[16].name);
    printf("  ── RPC 序列号 ──\n");
    printf("  [18] %s\n", kRouteTests[17].name);
    printf("  [19] %s\n", kRouteTests[18].name);
    printf("  [20] %s\n", kRouteTests[19].name);
    printf("  [21] %s\n", kRouteTests[20].name);
    printf("  ── 消息头结构 ──\n");
    printf("  [22] %s\n", kRouteTests[21].name);
    printf("  [23] %s\n", kRouteTests[22].name);
    printf("  [a] 运行全部\n");
    printf("  [b] 返回主菜单\n");
    printf("==================================\n");
    printf("> ");
    fflush(stdout);
}

void TestApp::HandleRouteCmd(char cmd)
{
    if (cmd == 'b' || cmd == 'B')
    {
        current_level_ = MainMenu;
        menu_drawn_ = false;
        return;
    }

    if (cmd == 'a' || cmd == 'A')
    {
        printf("  -> 运行全部路由测试...\n");
        fflush(stdout);
        int ret = RunForkedTest("[route]");
        if (ret == 0)
            printf("  -> 全部路由测试通过\n");
        else
            printf("  -> 路由测试失败 (exit=%d)\n", ret);
        fflush(stdout);
        menu_drawn_ = false;
        return;
    }

    printf("  未知命令: '%c'\n", cmd);
    menu_drawn_ = false;
}

void TestApp::RunSingleRouteTest(int index)
{
    if (index < 0 || index >= kRouteTestCount)
    {
        printf("  无效编号\n");
        return;
    }

    printf("  -> 运行: %s\n", kRouteTests[index].name);
    fflush(stdout);
    int ret = RunForkedTest(kRouteTests[index].name);
    if (ret == 0)
        printf("  -> 通过\n");
    else
        printf("  -> 失败 (exit=%d)\n", ret);
    fflush(stdout);
}

// ─── Scheduler Submenu ────────────────────────────────────────

void TestApp::PrintSchedulerMenu()
{
    printf("\n======= 调度器测试用例 (%d) =======\n", kSchedulerTestCount);
    for (int i = 0; i < kSchedulerTestCount; i++)
    {
        printf("  [%2d] %s\n", i + 1, kSchedulerTests[i].name);
    }
    printf("  [a] 运行全部\n");
    printf("  [b] 返回主菜单\n");
    printf("==================================\n");
    printf("> ");
    fflush(stdout);
}

void TestApp::HandleSchedulerCmd(char cmd)
{
    if (cmd == 'b' || cmd == 'B')
    {
        current_level_ = MainMenu;
        menu_drawn_ = false;
        return;
    }

    if (cmd == 'a' || cmd == 'A')
    {
        printf("  -> 运行全部调度器测试...\n");
        fflush(stdout);
        int passed = 0, failed = 0;

        auto run = [&](const char* name, std::function<int()> fn) {
            printf("  %s ... ", name);
            fflush(stdout);
            int r = fn();
            if (r == 0) { printf("通过\n"); passed++; }
            else        { printf("失败 (%d)\n", r); failed++; }
            fflush(stdout);
        };

        run(kSchedulerTests[0].name, [this]() { return RunWorkerExecutorDispatchTest(); });
        run(kSchedulerTests[1].name, [this]() { return RunWorkerExecutorMainTest(); });
        run(kSchedulerTests[2].name, [this]() { return RunThreadPoolDispatchTest(); });
        run(kSchedulerTests[3].name, [this]() { return RunThreadPoolPostTest(); });

        printf("  -> %d passed, %d failed\n", passed, failed);
        fflush(stdout);
        menu_drawn_ = false;
        return;
    }

    printf("  未知命令: '%c'\n", cmd);
    menu_drawn_ = false;
}

void TestApp::RunSingleSchedulerTest(int index)
{
    if (index < 0 || index >= kSchedulerTestCount)
    {
        printf("  无效编号\n");
        return;
    }

    printf("  -> 运行: %s\n", kSchedulerTests[index].name);
    fflush(stdout);

    std::function<int()> fns[] = {
        [this]() { return RunWorkerExecutorDispatchTest(); },
        [this]() { return RunWorkerExecutorMainTest(); },
        [this]() { return RunThreadPoolDispatchTest(); },
        [this]() { return RunThreadPoolPostTest(); },
    };

    int ret = fns[index]();
    if (ret == 0)
        printf("  -> 通过\n");
    else
        printf("  -> 失败 (exit=%d)\n", ret);
    fflush(stdout);
}

// ── 调度器集成测试（需 Worker 上下文，在父进程中运行）──

int TestApp::RunWorkerExecutorDispatchTest()
{
    auto worker = gb::WorkerManager::Instance()->GetWorker(1);
    if (!worker)
    {
        printf("  Worker(1) not available\n");
        return -1;
    }

    gb::WorkerExecutor exec(worker);
    if (!exec.HasWorker())
    {
        printf("  HasWorker() 返回 false\n");
        return -1;
    }

    std::atomic<int> result{0};
    std::mutex       mtx;
    std::condition_variable cv;
    bool done = false;

    bool ok = exec.Dispatch([&]() {
        result.store(42);
        {
            std::lock_guard<std::mutex> lk(mtx);
            done = true;
        }
        cv.notify_one();
    });
    if (!ok)
    {
        printf("  Dispatch 返回 false\n");
        return -1;
    }

    {
        std::unique_lock<std::mutex> lk(mtx);
        if (!cv.wait_for(lk, std::chrono::seconds(3), [&done] { return done; }))
        {
            printf("  Wait timeout (result=%d)\n", result.load());
            return -1;
        }
    }

    if (result.load() != 42)
    {
        printf("  Expected 42, got %d\n", result.load());
        return -1;
    }
    return 0;
}

int TestApp::RunWorkerExecutorMainTest()
{
    auto exec = gb::WorkerExecutor::Current();
    // 在主线程上调用时，IsCurrent() 为 true → 同步 inline 执行

    int result = 0;
    bool ok = exec.Dispatch([&]() {
        result = 100;
    });
    if (!ok)
    {
        printf("  Dispatch 返回 false\n");
        return -1;
    }

    if (result != 100)
    {
        printf("  Expected 100, got %d\n", result);
        return -1;
    }
    return 0;
}

/// 等待 main_worker 事件处理完毕（用于 ThreadPoolScheduler 回调回投到 main_worker 的等待）
static int WaitForMainWorker(std::atomic<bool>& done, int timeout_ms = 3000)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!done.load() && std::chrono::steady_clock::now() < deadline)
    {
        // ThreadPool Dispatch/Post 的回调通过 worker->Post() 投递到这里
        auto main_worker = gb::WorkerManager::Instance()->GetMainWorker();
        if (main_worker)
        {
            std::function<void()> task;
            while (main_worker->TryDequeueEvent(task))
            {
                if (task) task();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done.load() ? 0 : -1;
}

int TestApp::RunThreadPoolDispatchTest()
{
    std::atomic<int>  result{0};
    std::atomic<bool> done{false};

    gb::ThreadPoolScheduler::Instance()->Dispatch<int>(
        []() { return 42; },
        [&](int r) {
            result.store(r);
            done.store(true);
        }
    );

    if (WaitForMainWorker(done) != 0)
    {
        printf("  Timeout waiting for Dispatch callback (result=%d)\n", result.load());
        return -1;
    }

    if (result.load() != 42)
    {
        printf("  Expected 42, got %d\n", result.load());
        return -1;
    }
    return 0;
}

int TestApp::RunThreadPoolPostTest()
{
    std::atomic<bool> called{false};
    std::atomic<bool> done{false};

    gb::ThreadPoolScheduler::Instance()->Post([&]() {
        called.store(true);
        done.store(true);
    });

    if (WaitForMainWorker(done) != 0)
    {
        printf("  Timeout waiting for Post callback (called=%d)\n", called.load() ? 1 : 0);
        return -1;
    }

    if (!called.load())
    {
        printf("  Post callback not executed\n");
        return -1;
    }
    return 0;
}

// ─── Fork+Exec helpers ─────────────────────────────────────

int TestApp::RunForkedTest(const char* filter)
{
#ifdef _WIN32
    SetEnvironmentVariable("UNIT_TEST_HEADLESS", "1");

    // Build UTF-8 command line, then convert to UTF-16 for CreateProcessW
    // to avoid mojibake when passing Chinese filter strings through ANSI ACP
    std::string cmdLineUtf8 = "\"" + exe_path_ + "\"";
    if (filter && filter[0])
    {
        cmdLineUtf8 += " \"" + std::string(filter) + "\"";
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, cmdLineUtf8.c_str(), -1, nullptr, 0);
    std::wstring cmdLineW(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmdLineUtf8.c_str(), -1, &cmdLineW[0], wlen);

    STARTUPINFOW siW       = { sizeof(siW) };
    PROCESS_INFORMATION pi = {};

    if (CreateProcessW(nullptr, &cmdLineW[0], nullptr, nullptr,
                       FALSE, 0, nullptr, nullptr, &siW, &pi))
    {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return (int)exitCode;
    }
    printf("  -> CreateProcessW failed (%lu)\n", GetLastError());
    return -1;
#else
    pid_t pid = fork();
    if (pid == 0)
    {
        setenv("UNIT_TEST_HEADLESS", "1", 1);
        const char* argv[] = {exe_path_.c_str(), filter, nullptr};
        execvp(argv[0], const_cast<char* const*>(argv));
        _exit(1);
    }
    else if (pid > 0)
    {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status))
            return WEXITSTATUS(status);
        return -1;
    }
    printf("  -> fork 失败\n");
    return -1;
#endif
}

void TestApp::RunSingleMsgpackTest(int index)
{
    if (index < 0 || index >= kMsgpackTestCount)
    {
        printf("  无效编号\n");
        return;
    }

    printf("  -> 运行: %s\n", kMsgpackTests[index].name);
    fflush(stdout);
    int ret = RunForkedTest(kMsgpackTests[index].name);
    if (ret == 0)
        printf("  -> 通过\n");
    else
        printf("  -> 失败 (exit=%d)\n", ret);
    fflush(stdout);
}

// ─── Menu Dispatch ──────────────────────────────────────────

void TestApp::PrintMenu()
{
    switch (current_level_)
    {
    case MainMenu:       PrintMainMenu();       break;
    case MsgpackMenu:    PrintMsgpackMenu();    break;
    case RouteMenu:      PrintRouteMenu();      break;
    case SchedulerMenu:  PrintSchedulerMenu();  break;
    }
}

void TestApp::HandleMenuCommand(char cmd)
{
    switch (current_level_)
    {
    case MainMenu:       HandleMainCmd(cmd);       break;
    case MsgpackMenu:    HandleMsgpackCmd(cmd);    break;
    case RouteMenu:      HandleRouteCmd(cmd);      break;
    case SchedulerMenu:  HandleSchedulerCmd(cmd);  break;
    }
}

// ─── Frame Loop ─────────────────────────────────────────────

int TestApp::OnUpdate(float)
{
    if (!menu_drawn_)
    {
        PrintMenu();
        menu_drawn_ = true;
    }

    char buf[64];
    int n = 0;

#ifdef _WIN32
    if (_kbhit())
    {
        n = _read(0, buf, sizeof(buf) - 1);
    }
#else
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))
    {
        n = (int)read(STDIN_FILENO, buf, sizeof(buf) - 1);
    }
#endif

    if (n > 0)
    {
        buf[n] = '\0';
        for (int i = 0; i < n; i++)
        {
            char c = buf[i];
            if (c == '\n' || c == '\r')
                continue;

            if ((current_level_ == MsgpackMenu || current_level_ == RouteMenu || current_level_ == SchedulerMenu) && c >= '0' && c <= '9')
            {
                int num = 0;
                while (i < n && buf[i] >= '0' && buf[i] <= '9')
                {
                    num = num * 10 + (buf[i] - '0');
                    i++;
                }
                i--;
            if (current_level_ == MsgpackMenu && num >= 1 && num <= kMsgpackTestCount)
            {
                RunSingleMsgpackTest(num - 1);
                menu_drawn_ = false;
            }
            else if (current_level_ == RouteMenu && num >= 1 && num <= kRouteTestCount)
            {
                RunSingleRouteTest(num - 1);
                menu_drawn_ = false;
            }
            else if (current_level_ == SchedulerMenu && num >= 1 && num <= kSchedulerTestCount)
            {
                RunSingleSchedulerTest(num - 1);
                menu_drawn_ = false;
            }
            else
            {
                int max = (current_level_ == MsgpackMenu) ? kMsgpackTestCount
                        : (current_level_ == RouteMenu) ? kRouteTestCount
                        : kSchedulerTestCount;
                printf("  无效编号: %d (1-%d)\n", num, max);
                fflush(stdout);
                menu_drawn_ = false;
            }
                continue;
            }

            HandleMenuCommand(c);
        }
    }

    return 0;
}

int TestApp::OnTick()
{
    ++frame_count_;
    return menu_quit_ ? -1 : 0;
}

int TestApp::OnCleanup()
{
    LOG_INFO("TestApp::OnCleanup");
    auto workers = gb::WorkerManager::Instance()->GetWorkers();
    for (auto& w : workers)
    {
        if (w)
            w->OnCleanup();
    }
    return 0;
}
