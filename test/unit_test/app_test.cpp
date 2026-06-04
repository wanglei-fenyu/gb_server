#include "app_test.h"
#include "base/res_path.h"
#include "async/thread_pool_scheduler.h"
#include "msgpack/msgpack.hpp"
#include <poll.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>

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
    printf("  [6] msgpack 打解包演示 -- 打包/解包并显示二进制格式\n");
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

    case '6': {
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

// ─── Fork+Exec helpers ─────────────────────────────────────

int TestApp::RunForkedTest(const char* filter)
{
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
    case MainMenu:    PrintMainMenu();  break;
    case MsgpackMenu: PrintMsgpackMenu(); break;
    }
}

void TestApp::HandleMenuCommand(char cmd)
{
    switch (current_level_)
    {
    case MainMenu:    HandleMainCmd(cmd);    break;
    case MsgpackMenu: HandleMsgpackCmd(cmd); break;
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

    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))
    {
        char buf[64];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
        if (n > 0)
        {
            buf[n] = '\0';
            for (ssize_t i = 0; i < n; i++)
            {
                char c = buf[i];
                if (c == '\n' || c == '\r')
                    continue;

                if (current_level_ == MsgpackMenu && c >= '0' && c <= '9')
                {
                    int num = 0;
                    while (i < n && buf[i] >= '0' && buf[i] <= '9')
                    {
                        num = num * 10 + (buf[i] - '0');
                        i++;
                    }
                    i--;
                    if (num >= 1 && num <= kMsgpackTestCount)
                    {
                        RunSingleMsgpackTest(num - 1);
                        menu_drawn_ = false;
                    }
                    else
                    {
                        printf("  无效编号: %d (1-%d)\n", num, kMsgpackTestCount);
                        fflush(stdout);
                        menu_drawn_ = false;
                    }
                    continue;
                }

                HandleMenuCommand(c);
            }
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
