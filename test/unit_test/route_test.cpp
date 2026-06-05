//
// route_test.cpp — 路由模块单元测试：实体路由、服务路由、RPC 序列号
//
// 所有测试不依赖 App/Worker/Lua 初始化，可在 UNIT_TEST_HEADLESS 子进程中独立运行。
//
#include <catch2/catch_test_macros.hpp>

// Catch2 的 CHECK 与项目 log.h 的 CHECK 冲突，取消 Catch2 版本
#ifdef CHECK
#undef CHECK
#endif

#include "network/router/lock_free_route_table.h"
#include "network/router/route_table.h"
#include "network/router/router.h"
#include "network/router/message_type.h"
#include "network/io/message_meta.h"
#include "network/rpc/rpc_call.h"

#include <thread>
#include <vector>

using namespace gb;

// ============================================================
// LockFreeRouteTable — 无锁实体路由表核心操作
// ============================================================

TEST_CASE("route: 空表 Lookup 返回 kInvalidWorker", "[route][lockfree]")
{
    LockFreeRouteTable table;
    REQUIRE(table.Lookup(0) == LockFreeRouteTable::kInvalidWorker);
    REQUIRE(table.Lookup(UINT64_MAX) == LockFreeRouteTable::kInvalidWorker);
    REQUIRE(table.IsEmpty() == true);
}

TEST_CASE("route: Bind 单例并能 Lookup", "[route][lockfree]")
{
    LockFreeRouteTable table;
    table.Bind(100, 101, 1);
    table.Freeze();
    REQUIRE(table.Lookup(100) == 1);
    REQUIRE(table.Lookup(99) == LockFreeRouteTable::kInvalidWorker);
    REQUIRE(table.Lookup(101) == LockFreeRouteTable::kInvalidWorker);
}

TEST_CASE("route: Bind 区间并在区间内查找", "[route][lockfree]")
{
    LockFreeRouteTable table;
    table.Bind(100, 200, 2);
    table.Freeze();
    for (uint64_t e = 100; e < 200; e++)
        REQUIRE(table.Lookup(e) == 2);
}

TEST_CASE("route: Bind 区间区间外返回 Invalid", "[route][lockfree]")
{
    LockFreeRouteTable table;
    table.Bind(100, 200, 2);
    table.Freeze();
    REQUIRE(table.Lookup(99) == LockFreeRouteTable::kInvalidWorker);
    REQUIRE(table.Lookup(200) == LockFreeRouteTable::kInvalidWorker);
    REQUIRE(table.Lookup(999) == LockFreeRouteTable::kInvalidWorker);
}

TEST_CASE("route: 重叠 Bind 覆盖旧条目", "[route][lockfree]")
{
    LockFreeRouteTable table;

    // 先绑大区间
    table.Bind(0, 1000, 1);
    table.Freeze();
    REQUIRE(table.Lookup(500) == 1);

    // 再绑小区间覆盖中间部分
    table.Bind(400, 600, 2);
    table.Freeze();
    REQUIRE(table.Lookup(300) == 1);   // 左段保留原 worker
    REQUIRE(table.Lookup(500) == 2);   // 中间被覆盖
    REQUIRE(table.Lookup(700) == 1);   // 右段保留原 worker
}

TEST_CASE("route: Unbind 移除单例", "[route][lockfree]")
{
    LockFreeRouteTable table;
    table.Bind(100, 101, 1);
    table.Freeze();
    REQUIRE(table.Lookup(100) == 1);

    table.Unbind(100);
    table.Freeze();
    REQUIRE(table.Lookup(100) == LockFreeRouteTable::kInvalidWorker);
}

TEST_CASE("route: Unbind 从区间中间拆分", "[route][lockfree]")
{
    LockFreeRouteTable table;
    table.Bind(0, 10, 1);
    table.Freeze();

    // Unbind entity 5 → 应该拆成 [0,5) 和 [6,10)
    table.Unbind(5);
    table.Freeze();

    REQUIRE(table.Lookup(4) == 1);
    REQUIRE(table.Lookup(5) == LockFreeRouteTable::kInvalidWorker);
    REQUIRE(table.Lookup(6) == 1);
}

TEST_CASE("route: Unbind 区间开头", "[route][lockfree]")
{
    LockFreeRouteTable table;
    table.Bind(0, 10, 1);
    table.Freeze();

    table.Unbind(0);
    table.Freeze();
    REQUIRE(table.Lookup(0) == LockFreeRouteTable::kInvalidWorker);
    REQUIRE(table.Lookup(1) == 1);
}

TEST_CASE("route: Unbind 区间末尾", "[route][lockfree]")
{
    LockFreeRouteTable table;
    table.Bind(0, 10, 1);
    table.Freeze();

    table.Unbind(9);
    table.Freeze();
    REQUIRE(table.Lookup(8) == 1);
    REQUIRE(table.Lookup(9) == LockFreeRouteTable::kInvalidWorker);
}

TEST_CASE("route: Freeze 发布多条目变更", "[route][lockfree]")
{
    LockFreeRouteTable table;

    table.Bind(10, 20, 1);
    table.Bind(30, 40, 2);
    table.Bind(50, 60, 3);
    table.Freeze();

    REQUIRE(table.Lookup(15) == 1);
    REQUIRE(table.Lookup(35) == 2);
    REQUIRE(table.Lookup(55) == 3);
    REQUIRE(table.Lookup(25) == LockFreeRouteTable::kInvalidWorker);
}

// ============================================================
// RouteTable — 服务路由类型解析
// ============================================================

TEST_CASE("route: 默认 resolver 返回 SWT_Normal", "[route][routetable]")
{
    RouteTable table;
    // 未设置 resolver → 默认返回 SWT_Normal
    REQUIRE(table.ResolveServiceWorkerType(MT_Login) == SWT_Normal);
    REQUIRE(table.ResolveServiceWorkerType(MT_AI_Run) == SWT_Normal);
    REQUIRE(table.ResolveServiceWorkerType(static_cast<MessageType>(99999)) == SWT_Normal);
}

TEST_CASE("route: 自定义 resolver 映射消息类型", "[route][routetable]")
{
    RouteTable table;
    table.SetServiceTypeResolver([](MessageType type) -> ServiceWorkerType {
        if (type >= 10000 && type < 20000) return SWT_AI;
        if (type >= 20000 && type < 30000) return SWT_Navigation;
        return SWT_Normal;
    });

    REQUIRE(table.ResolveServiceWorkerType(MT_Login) == SWT_Normal);
    REQUIRE(table.ResolveServiceWorkerType(MT_AI_Skill) == SWT_AI);
    REQUIRE(table.ResolveServiceWorkerType(MT_LineFindPath) == SWT_Navigation);
}

TEST_CASE("route: RegisterWorker + GetWorker 一对一", "[route][routetable]")
{
    RouteTable table;
    // 没有实际 Worker 实例，但 SharedPtr 可以为空测试接口
    std::shared_ptr<Worker> empty;
    table.RegisterWorker(SWT_Normal, empty);

    auto workers = table.GetWorker(SWT_Normal);
    REQUIRE(workers.size() == 1);
    REQUIRE(workers[0].expired());   // 空 shared_ptr 构造的 weak_ptr 已过期
}

TEST_CASE("route: RegisterWorker 多 worker 注册", "[route][routetable]")
{
    RouteTable table;
    std::shared_ptr<Worker> w1, w2, w3;
    table.RegisterWorker(SWT_Normal, w1);
    table.RegisterWorker(SWT_Normal, w2);
    table.RegisterWorker(SWT_AI, w3);

    REQUIRE(table.GetWorker(SWT_Normal).size() == 2);
    REQUIRE(table.GetWorker(SWT_AI).size() == 1);
    REQUIRE(table.GetWorker(SWT_Navigation).size() == 0);
}

// ============================================================
// Router — 实体路由 + 服务路由调度
// ============================================================

TEST_CASE("route: Router::GetEntityExecutor 未绑定丢弃（Stateful）", "[route][router]")
{
    Router router;
    router.SetPolicy(Router::Policy::Stateful);

    // Stateful + 未绑定 + 无 workers → hash 返回空 executor（丢弃）
    auto exec = router.GetExecutor(/*type=*/0, /*entity_id=*/42);
    REQUIRE(exec.HasWorker() == false);

    // 绑定到不存在的 worker index（9999）→ worker 不存在，丢弃
    router.BindEntity(10, 20, 9999);
    router.FreezeEntityRoutes();
    exec = router.GetExecutor(/*type=*/0, /*entity_id=*/15);
    REQUIRE(exec.HasWorker() == false);
    REQUIRE(router.GetEntityRouteTable().Lookup(15) == 9999);
}

TEST_CASE("route: Router::GetServiceExecutor entity_id==0 路由到 main_worker_", "[route][router]")
{
    Router router;
    // entity_id == 0：系统消息 → main_worker_（即使没有注册任何 Worker）
    auto exec = router.GetServiceExecutor(MT_Login, 0);
    REQUIRE(exec.HasWorker() == true);
}

TEST_CASE("route: Router::GetExecutor(Stateful) entity_id==0 路由到 main_worker_", "[route][router]")
{
    Router router;
    router.SetPolicy(Router::Policy::Stateful);
    // Stateful 下 entity_id == 0 也走 main_worker_
    REQUIRE(router.GetExecutor(/*type=*/0, /*entity_id=*/0).HasWorker() == true);
}

TEST_CASE("route: Router::Bind→Freeze→GetExecutor(Stateful) 验证路由表", "[route][router]")
{
    Router router;
    router.SetPolicy(Router::Policy::Stateful);

    router.BindEntity(100, 200, 99);
    router.BindEntity(300, 400, 88);
    router.FreezeEntityRoutes();

    // 未绑定的 entity → 无 workers，丢弃
    REQUIRE(router.GetExecutor(/*type=*/0, /*entity_id=*/250).HasWorker() == false);
    REQUIRE(router.GetExecutor(/*type=*/0, /*entity_id=*/500).HasWorker() == false);

    // 已绑定但 worker index 99/88 不存在 → 丢弃
    REQUIRE(router.GetExecutor(/*type=*/0, /*entity_id=*/150).HasWorker() == false);
    REQUIRE(router.GetExecutor(/*type=*/0, /*entity_id=*/350).HasWorker() == false);

    // 验证路由表内容正确（不经过 WorkerManager）
    const auto& tbl = router.GetEntityRouteTable();
    REQUIRE(tbl.Lookup(150) == 99);
    REQUIRE(tbl.Lookup(350) == 88);
    REQUIRE(tbl.Lookup(250) == LockFreeRouteTable::kInvalidWorker);
    REQUIRE(tbl.Lookup(100) == 99);
    REQUIRE(tbl.Lookup(199) == 99);
    REQUIRE(tbl.Lookup(300) == 88);
    REQUIRE(tbl.Lookup(399) == 88);
}

// ============================================================
// SequenceId — RPC 序列号编解码
// ============================================================

TEST_CASE("route: SequenceId 编解码往返", "[route][sequence]")
{
    SequenceId sid;
    sid.index = 3;
    sid.seq   = 42;

    uint64_t encoded = sid.value;

    SequenceId decoded;
    decoded.value = encoded;

    REQUIRE(decoded.index == 3);
    REQUIRE(decoded.seq == 42);
}

TEST_CASE("route: SequenceId 零值", "[route][sequence]")
{
    SequenceId sid;
    sid.value = 0;
    REQUIRE(sid.index == 0);
    REQUIRE(sid.seq == 0);
}

TEST_CASE("route: SequenceId 边界值", "[route][sequence]")
{
    SequenceId sid;
    sid.index = 0xFFFFFFFF;
    sid.seq   = 0xFFFFFFFF;
    REQUIRE(sid.value == 0xFFFFFFFFFFFFFFFFULL);

    SequenceId sid2;
    sid2.index = 0xFFFFFFFF;
    sid2.seq   = 0;
    // GCC/Clang: 第一个声明的 index 在低 32 位
    REQUIRE(sid2.value == 0xFFFFFFFFULL);
}

TEST_CASE("route: SequenceId worker_index 编码偏移", "[route][sequence]")
{
    // GCC/Clang bitfield 布局：index 在低 32 位，seq 在高 32 位
    SequenceId sid;
    sid.index = 5;
    sid.seq   = 0;
    REQUIRE(sid.value == 5);

    sid.index = 0;
    sid.seq   = 5;
    REQUIRE(sid.value == static_cast<uint64_t>(5) << 32);

    sid.index = 7;
    sid.seq   = 99;
    REQUIRE(sid.value == ((static_cast<uint64_t>(99) << 32) | 7));
}

// ============================================================
// Meta / MsgMode — 消息头结构
// ============================================================

TEST_CASE("route: Meta 默认构造各字段为零", "[route][meta]")
{
    Meta meta;
    REQUIRE(meta.mode == Msg);
    REQUIRE(meta.entity_id == 0);
    REQUIRE(meta.type == 0);
    REQUIRE(meta.compress_type == CompressTypeNone);
    REQUIRE(meta.method == 0);
    REQUIRE(meta.sequence == 0);
}

TEST_CASE("route: MsgMode 枚举值定义", "[route][meta]")
{
    REQUIRE(Msg == 0);
    REQUIRE(Request == 1);
    REQUIRE(Response == 2);
}

TEST_CASE("route: Meta 手工构造完整验证", "[route][meta]")
{
    Meta meta;
    meta.mode = Request;
    meta.entity_id = 12345;
    meta.type = 10001;
    meta.compress_type = CompressTypeNone;
    meta.method = 0xDEADBEEF;
    meta.sequence = 0xABCD000000000042ULL;

    REQUIRE(meta.mode == Request);
    REQUIRE(meta.entity_id == 12345);
    REQUIRE(meta.type == 10001);
    REQUIRE(meta.compress_type == CompressTypeNone);
    REQUIRE(meta.method == 0xDEADBEEF);
    REQUIRE(meta.sequence == 0xABCD000000000042ULL);
}
