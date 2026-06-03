/// db_test — 独立的数据库/Redis 集成测试。
///
/// 用法：
///   运行后显示菜单，输入数字选择测试项，Enter 执行。
///   每个测试是独立的，连接在测试内部创建和销毁。
///
/// 结果文件：
///   所有测试结果写入 log4/db_test_report.txt，每条包含：
///     标题 | 结果 | PASS/FAIL
///
/// 编译：
///   与 server_test 共享 src/ 下的全部源码，但不启动 App 框架，
///   没有 Worker、NetworkManager 或 Lua 状态。

import db.redis;
#include "log/log.h"

import db.postgres;
#include "report.h"
#include <iostream>
#include <string>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

// ── 全局报告实例 ──
TestReporter g_reporter("log4/db_test_report.txt");

// ── 前置声明 ──
int MenuTestRedisPing();
int MenuTestRedisKV();
int MenuTestRedisHash();
int MenuTestRedisList();
int MenuTestRedisZSet();
int MenuTestRedisZSetRange();
int MenuTestRedisZSetAdv();
int MenuTestRedisLuaScript();
int MenuTestRedisExpire();
int MenuTestRedisAsyncCallback();
int MenuTestRedisAsyncCallEval();
int MenuTestRedisErrorCases();
int MenuTestRedisLifecycle();
int MenuTestPgConnectClose();
int MenuTestPgQuery();
int MenuTestPgExecute();
int MenuTestPgTransaction();
int MenuTestPgSubquery();
int MenuTestPgJoin();
int MenuTestPgCoroQuery();
int MenuTestPgCoroTransaction();
int MenuTestLuaScriptBindings();

// ── 日志辅助 ──
static void PrintSection(const std::string& title)
{
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "  " << title << "\n";
    std::cout << std::string(60, '=') << "\n" << std::endl;
}

static void PrintResult(const std::string& label, bool passed,
                         const std::string& detail = "")
{
    std::cout << (passed ? "[PASS]" : "[FAIL]") << " " << label;
    if (!detail.empty()) std::cout << " -- " << detail;
    std::cout << std::endl;
}

// ====================================================================
// 主菜单
// ====================================================================

struct MenuItem {
    int         id;
    std::string label;
    int (*func)();
};

static const MenuItem MENU[] = {
    // ── Redis 测试 (13 项) ──
    { 1,  "Redis Ping",                          MenuTestRedisPing },
    { 2,  "Redis KV (Set/Get/Del/Exists/Incr)",  MenuTestRedisKV },
    { 3,  "Redis Hash (HSet/HGet/HDel/HLen)",    MenuTestRedisHash },
    { 4,  "Redis List (LPush/RPush/LPop/RPop)",  MenuTestRedisList },
    { 5,  "Redis Sorted Set (ZAdd/ZCard/ZScore)", MenuTestRedisZSet },
    { 6,  "Redis ZSET Range (index+score verify)",MenuTestRedisZSetRange },
    { 7,  "Redis ZSET Advanced (Co*/Async*)",    MenuTestRedisZSetAdv },
    { 8,  "Redis Lua Script (redis.call)",        MenuTestRedisLuaScript },
    { 9,  "Redis Expire / TTL",                  MenuTestRedisExpire },
    { 10, "Redis Async Callback API",            MenuTestRedisAsyncCallback },
    { 11, "Redis AsyncCall / AsyncEval",         MenuTestRedisAsyncCallEval },
    { 12, "Redis Error Handling",                MenuTestRedisErrorCases },
    { 13, "Redis Connection Lifecycle",          MenuTestRedisLifecycle },
    // ── PostgreSQL 测试 (8 项) ──
    { 14, "PG AsyncConnect / AsyncClose",        MenuTestPgConnectClose },
    { 15, "PG AsyncQuery",                       MenuTestPgQuery },
    { 16, "PG AsyncExecute",                     MenuTestPgExecute },
    { 17, "PG Transaction (Begin/Commit/Rollback)", MenuTestPgTransaction },
    { 18, "PG Subquery (IN, EXISTS)",            MenuTestPgSubquery },
    { 19, "PG JOIN (INNER, LEFT, RIGHT, UNION)", MenuTestPgJoin },
    { 20, "PG Coroutine Query/Execute",          MenuTestPgCoroQuery },
    { 21, "PG Coroutine Transaction",            MenuTestPgCoroTransaction },
    // ── Lua Script 测试 (1 项) ──
    { 22, "Lua Redis/PG Bindings",              MenuTestLuaScriptBindings },
};

static constexpr int REDIS_COUNT = 13;
static constexpr int PG_COUNT    = 8;
static constexpr int LUA_COUNT   = 1;
static constexpr int MENU_COUNT  = REDIS_COUNT + PG_COUNT + LUA_COUNT;  // 22

static void ShowMenu()
{
    std::cout << "\n"
              << "+------------------------------------------+\n"
              << "|        DB / Redis 集成测试菜单           |\n"
              << "+------------------------------------------+\n"
              << "|  Redis 测试:                             |\n";
    for (int i = 0; i < MENU_COUNT; ++i)
    {
        if (i == REDIS_COUNT)
            std::cout << "|  PostgreSQL 测试:                        |\n";
        if (i == REDIS_COUNT + PG_COUNT)
            std::cout << "|  Lua Script 测试:                        |\n";
        std::cout << "|    " << MENU[i].id << ". " << MENU[i].label;
        // pad to 37 chars (including the 4-char prefix)
        int pad = 37 - (4 + (int)MENU[i].label.size());
        if (pad < 1) pad = 1;
        std::cout << std::string(pad, ' ') << "|\n";
    }
    std::cout << "|                                       |\n"
              << "|    0. 退出                              |\n"
              << "|    a. 运行全部 Redis 测试                |\n"
              << "|    b. 运行全部 PostgreSQL 测试           |\n"
              << "|    c. 运行所有测试                       |\n"
              << "|    d. 运行全部 Lua Script 测试           |\n"
              << "+------------------------------------------+\n"
              << "请选择: ";
}

int main()
{
#ifdef _WIN32
    // 设置控制台为 UTF-8 编码，确保中文菜单正常显示
    SetConsoleOutputCP(65001);
#endif

    // 初始化日志（写到文件，控制台不输出日志，避免干扰菜单）
    GbLog db_log;
    db_log.Init("log4/db_test.log", 1024 * 1024 * 100, 10,
                GbLog::ASYNC, GbLog::FILE, GbLog::LEVEL_INFO);

    std::cout << "db_test — 数据库/Redis 集成测试" << std::endl;
    std::cout << "日志文件: log4/db_test.log" << std::endl;
    std::cout << "报告文件: log4/db_test_report.txt" << std::endl;

    while (true)
    {
        ShowMenu();
        std::string input;
        std::getline(std::cin, input);

        if (input.empty()) continue;

        // ── 特殊命令 ──
        if (input == "0") { std::cout << "bye\n"; break; }

        if (input == "a")
        {
            int passed = 0, failed = 0;
            for (int i = 0; i < REDIS_COUNT; ++i)
            {
                int r = MENU[i].func();
                if (r == 0) ++passed; else ++failed;
            }
            std::cout << "\nRedis 全部: 通过 " << passed
                      << ", 失败 " << failed << "\n";
            continue;
        }

        if (input == "b")
        {
            int passed = 0, failed = 0;
            for (int i = REDIS_COUNT; i < MENU_COUNT; ++i)
            {
                int r = MENU[i].func();
                if (r == 0) ++passed; else ++failed;
            }
            std::cout << "\nPostgreSQL 全部: 通过 " << passed
                      << ", 失败 " << failed << "\n";
            continue;
        }

        if (input == "c")
        {
            int passed = 0, failed = 0;
            for (int i = 0; i < MENU_COUNT; ++i)
            {
                int r = MENU[i].func();
                if (r == 0) ++passed; else ++failed;
            }
            std::cout << "\n全部: 通过 " << passed
                      << ", 失败 " << failed << "\n";
            continue;
        }

        if (input == "d")
        {
            int passed = 0, failed = 0;
            int start = REDIS_COUNT + PG_COUNT;
            for (int i = start; i < MENU_COUNT; ++i)
            {
                int r = MENU[i].func();
                if (r == 0) ++passed; else ++failed;
            }
            std::cout << "\nLua Script 全部: 通过 " << passed
                      << ", 失败 " << failed << "\n";
            continue;
        }

        // ── 数字选择 ──
        int choice = 0;
        try { choice = std::stoi(input); } catch (...) { continue; }

        bool found = false;
        for (int i = 0; i < MENU_COUNT; ++i)
        {
            if (MENU[i].id == choice)
            {
                MENU[i].func();
                found = true;
                break;
            }
        }
        if (!found)
            std::cout << "无效选项\n";
    }

    return 0;
}
