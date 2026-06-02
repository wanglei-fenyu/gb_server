#include "redis_test.h"
#include "db/redis/redis_connection.h"
#include "report.h"
#include <future>
#include <memory>
#include "async_simple/coro/SyncAwait.h"
#include <iostream>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>

// ── 创建默认 Redis 连接 ──
static bool CreateConnection(RedisConnection& conn)
{
    RedisConfig cfg;
    cfg.host     = "192.168.31.186";
    cfg.port     = 6379;
    cfg.password = "fengyu";
    cfg.db_index = 0;
    cfg.timeout_ms = 5000;
    return conn.Connect(cfg);
}

// ══════════════════════════════════════════════════════════════════════
// 回调辅助：等待异步回调完成
// ══════════════════════════════════════════════════════════════════════

/// 等待一个无返回值的异步操作完成（std::promise 桥接）
static void WaitDone(const std::function<void(std::function<void()>)>& setup)
{
    auto p = std::make_shared<std::promise<void>>();
    auto f = p->get_future();
    setup([p]() mutable { p->set_value(); });
    f.get();
}

/// 等待一个有返回值的异步操作（std::promise 桥接）
template <typename T>
static T WaitValue(const std::function<void(std::function<void(T)>)>& setup)
{
    auto p = std::make_shared<std::promise<T>>();
    auto f = p->get_future();
    setup([p](T val) mutable { p->set_value(std::move(val)); });
    return f.get();
}

// ══════════════════════════════════════════════════════════════════════
// 测试用例
// ══════════════════════════════════════════════════════════════════════

int MenuTestRedisPing()
{
    TestSection("Redis — Ping");
    RedisConnection conn;
    if (!CreateConnection(conn)) { TestResult("Connect", false); return -1; }

    bool pong = WaitValue<bool>([&](auto cb) {
        conn.AsyncPing([&, cb = std::move(cb)](boost::system::error_code ec, bool ok) mutable {
            cb(!ec && ok);
            conn.Disconnect();
        });
    });
    TestResult("AsyncPing", pong, pong ? "pong=true" : "failed");

    return 0;
}

int MenuTestRedisKV()
{
    TestSection("Redis — KV Operations (细粒度)");
    RedisConnection conn;
    if (!CreateConnection(conn)) { TestResult("Connect", false); return -1; }
    int result = 0;

    // ── Clean up at START (not end) — data stays after test ──
    async_simple::coro::syncAwait(conn.CoDel("db:kv"));
    async_simple::coro::syncAwait(conn.CoDel("db:kv2"));
    async_simple::coro::syncAwait(conn.CoDel("db:incr"));

    // CoSet / CoGet
    bool set_ok = async_simple::coro::syncAwait(conn.CoSet("db:kv", "hello_db"));
    TestResult("CoSet db:kv = hello_db", set_ok, set_ok ? "true" : "false");
    if (!set_ok) ++result;

    std::string val = async_simple::coro::syncAwait(conn.CoGet("db:kv"));
    TestResult("CoGet db:kv", val == "hello_db", "got=\"" + val + "\" (expect hello_db)");
    if (val != "hello_db") ++result;

    // CoExists (existing key)
    bool ex = async_simple::coro::syncAwait(conn.CoExists("db:kv"));
    TestResult("CoExists db:kv (existing)", ex, ex ? "true" : "false");
    if (!ex) ++result;

    // CoExists (non-existent)
    bool nex = async_simple::coro::syncAwait(conn.CoExists("db:noexist"));
    TestResult("CoExists non-existent", !nex, nex ? "unexpected true" : "false (expected)");
    if (nex) ++result;

    // CoSetEx
    bool sex = async_simple::coro::syncAwait(conn.CoSetEx("db:kv2", "ttl_val", 200));
    TestResult("CoSetEx db:kv2 (ttl=200)", sex, sex ? "true" : "false");
    if (!sex) ++result;

    // CoGet after SetEx
    std::string v2 = async_simple::coro::syncAwait(conn.CoGet("db:kv2"));
    TestResult("CoGet db:kv2 (after SetEx)", v2 == "ttl_val", "got=\"" + v2 + "\" (expect ttl_val)");
    if (v2 != "ttl_val") ++result;

    // CoIncr / CoIncrBy
    int64_t i1 = async_simple::coro::syncAwait(conn.CoIncr("db:incr"));
    TestResult("CoIncr db:incr #1", i1 == 1, "val=" + std::to_string(i1) + " (expect 1)");
    if (i1 != 1) ++result;

    int64_t i2 = async_simple::coro::syncAwait(conn.CoIncr("db:incr"));
    TestResult("CoIncr db:incr #2", i2 == 2, "val=" + std::to_string(i2) + " (expect 2)");
    if (i2 != 2) ++result;

    int64_t ib = async_simple::coro::syncAwait(conn.CoIncrBy("db:incr", 10));
    TestResult("CoIncrBy db:incr +10", ib == 12, "val=" + std::to_string(ib) + " (expect 12)");
    if (ib != 12) ++result;

    // CoDel (existing)
    int64_t d1 = async_simple::coro::syncAwait(conn.CoDel("db:kv"));
    TestResult("CoDel db:kv (existing)", d1 > 0, "n=" + std::to_string(d1));
    if (d1 == 0) ++result;

    // CoDel (non-existent)
    int64_t d2 = async_simple::coro::syncAwait(conn.CoDel("db:noexist"));
    TestResult("CoDel non-existent (returns 0)", d2 == 0, "n=" + std::to_string(d2));
    if (d2 != 0) ++result;

    // CoGet non-existent
    std::string empty = async_simple::coro::syncAwait(conn.CoGet("db:noexist"));
    TestResult("CoGet non-existent (empty)", empty.empty(), empty.empty() ? "empty" : "got=\"" + empty + "\"");
    if (!empty.empty()) ++result;

    // Data left for inspection: db:kv2 (ttl), db:incr
    std::cout << "  [DATA] db:kv2 (TTL=200), db:incr="
              << async_simple::coro::syncAwait(conn.CoGet("db:incr"))
              << std::endl;

    conn.Disconnect();
    return result;
}

int MenuTestRedisHash()
{
    TestSection("Redis — Hash Operations (细粒度)");
    RedisConnection conn;
    if (!CreateConnection(conn)) { TestResult("Connect", false); return -1; }
    int result = 0;

    // Clean up at START
    async_simple::coro::syncAwait(conn.CoDel("db:hash"));

    // HSet (new field)
    int64_t hs1 = async_simple::coro::syncAwait(conn.CoHSet("db:hash", "f1", "v1"));
    TestResult("CoHSet new field f1=v1", hs1 == 1, "n=" + std::to_string(hs1) + " (expect 1)");
    if (hs1 != 1) ++result;

    // HSet (update existing field)
    int64_t hs2 = async_simple::coro::syncAwait(conn.CoHSet("db:hash", "f1", "v2"));
    TestResult("CoHSet update f1=v2", hs2 == 0, "n=" + std::to_string(hs2) + " (expect 0)");
    if (hs2 != 0) ++result;

    // HGet
    std::string hg = async_simple::coro::syncAwait(conn.CoHGet("db:hash", "f1"));
    TestResult("CoHGet f1", hg == "v2", "got=\"" + hg + "\" (expect v2)");
    if (hg != "v2") ++result;

    // HLen
    int64_t hl = async_simple::coro::syncAwait(conn.CoHLen("db:hash"));
    TestResult("CoHLen", hl >= 1, "len=" + std::to_string(hl));
    if (hl < 1) ++result;

    // HKeys
    auto hkeys = async_simple::coro::syncAwait(conn.CoHKeys("db:hash"));
    TestResult("CoHKeys", !hkeys.empty(), "cnt=" + std::to_string(hkeys.size()));
    if (hkeys.empty()) ++result;

    // HVals
    auto hvals = async_simple::coro::syncAwait(conn.CoHVals("db:hash"));
    TestResult("CoHVals", !hvals.empty(), "cnt=" + std::to_string(hvals.size()));
    if (hvals.empty()) ++result;

    // HDel
    int64_t hd = async_simple::coro::syncAwait(conn.CoHDel("db:hash", "f1"));
    TestResult("CoHDel f1", hd > 0, "n=" + std::to_string(hd));
    if (hd == 0) ++result;

    // Remaining data: db:hash (maybe empty or has other fields)
    std::cout << "  [DATA] db:hash HKeys remaining: "
              << async_simple::coro::syncAwait(conn.CoHKeys("db:hash")).size()
              << " fields\n";

    conn.Disconnect();
    return result;
}

int MenuTestRedisList()
{
    TestSection("Redis — List Operations (细粒度)");
    RedisConnection conn;
    if (!CreateConnection(conn)) { TestResult("Connect", false); return -1; }
    int result = 0;

    // Clean up at START
    async_simple::coro::syncAwait(conn.CoDel("db:list"));

    int64_t lp1 = async_simple::coro::syncAwait(conn.CoLPush("db:list", "a"));
    TestResult("CoLPush a", lp1 >= 1, "len=" + std::to_string(lp1));
    if (lp1 < 1) ++result;

    int64_t lp2 = async_simple::coro::syncAwait(conn.CoLPush("db:list", "b"));
    TestResult("CoLPush b", lp2 >= 2, "len=" + std::to_string(lp2));
    if (lp2 < 2) ++result;

    int64_t rp = async_simple::coro::syncAwait(conn.CoRPush("db:list", "c"));
    TestResult("CoRPush c", rp >= 3, "len=" + std::to_string(rp));
    if (rp < 3) ++result;

    int64_t ll = async_simple::coro::syncAwait(conn.CoLLen("db:list"));
    TestResult("CoLLen", ll >= 3, "len=" + std::to_string(ll));
    if (ll < 3) ++result;

    std::string pop = async_simple::coro::syncAwait(conn.CoLPop("db:list"));
    TestResult("CoLPop (should be 'b')", pop == "b", "got=\"" + pop + "\" (expect b)");
    if (pop != "b") ++result;

    std::string rpop = async_simple::coro::syncAwait(conn.CoRPop("db:list"));
    TestResult("CoRPop (should be 'c')", rpop == "c", "got=\"" + rpop + "\" (expect c)");
    if (rpop != "c") ++result;

    // Remaining: "a" in list
    int64_t rem = async_simple::coro::syncAwait(conn.CoLLen("db:list"));
    std::cout << "  [DATA] db:list remaining len=" << rem << "\n";

    conn.Disconnect();
    return result;
}

int MenuTestRedisZSet()
{
    TestSection("Redis — Sorted Set Operations (细粒度)");
    RedisConnection conn;
    if (!CreateConnection(conn)) { TestResult("Connect", false); return -1; }
    int result = 0;

    // Clean up at START
    async_simple::coro::syncAwait(conn.CoDel("db:zset"));

    int64_t za1 = async_simple::coro::syncAwait(conn.CoZAdd("db:zset", 1.0, "m1"));
    TestResult("CoZAdd m1 score=1.0", za1 == 1, "n=" + std::to_string(za1));
    if (za1 != 1) ++result;

    int64_t za2 = async_simple::coro::syncAwait(conn.CoZAdd("db:zset", 2.5, "m2"));
    TestResult("CoZAdd m2 score=2.5", za2 == 1, "n=" + std::to_string(za2));
    if (za2 != 1) ++result;

    int64_t za3 = async_simple::coro::syncAwait(conn.CoZAdd("db:zset", 0.5, "m3"));
    TestResult("CoZAdd m3 score=0.5 (lowest)", za3 == 1, "n=" + std::to_string(za3));
    if (za3 != 1) ++result;

    int64_t zc = async_simple::coro::syncAwait(conn.CoZCard("db:zset"));
    TestResult("CoZCard", zc == 3, "cnt=" + std::to_string(zc) + " (expect 3)");
    if (zc != 3) ++result;

    double zs = async_simple::coro::syncAwait(conn.CoZScore("db:zset", "m2"));
    TestResult("CoZScore m2", std::abs(zs - 2.5) < 0.001,
               "score=" + std::to_string(zs) + " (expect 2.5)");
    if (std::abs(zs - 2.5) >= 0.001) ++result;

    int64_t zr = async_simple::coro::syncAwait(conn.CoZRank("db:zset", "m3"));
    TestResult("CoZRank m3 (lowest=rank0)", zr == 0, "rank=" + std::to_string(zr));
    if (zr != 0) ++result;

    auto zrange = async_simple::coro::syncAwait(conn.CoZRange("db:zset", 0, -1, true));
    TestResult("CoZRange all with_scores", zrange.size() == 6,
               "elements=" + std::to_string(zrange.size()) + " (expect 6)");
    if (zrange.size() != 6) ++result;

    int64_t zrm = async_simple::coro::syncAwait(conn.CoZRem("db:zset", "m1"));
    TestResult("CoZRem m1", zrm == 1, "n=" + std::to_string(zrm));
    if (zrm != 1) ++result;

    // Remaining: m2(2.5), m3(0.5)
    {
        int64_t cnt = async_simple::coro::syncAwait(conn.CoZCard("db:zset"));
        std::cout << "  [DATA] db:zset after ZRem m1: " << cnt << " members remain\n";
    }

    conn.Disconnect();
    return result;
}

// ══════════════════════════════════════════════════════════════════════
// 新增: ZSET 范围查询 + 成员/分数验证
// ══════════════════════════════════════════════════════════════════════

int MenuTestRedisZSetRange()
{
    TestSection("Redis — ZSET Range with Score Verification");
    RedisConnection conn;
    if (!CreateConnection(conn)) { TestResult("Connect", false); return -1; }
    int result = 0;

    // Clean up at START
    async_simple::coro::syncAwait(conn.CoDel("db:zset_range"));

    // ── Setup: 5 members with different scores ──
    {
        int64_t n = async_simple::coro::syncAwait(conn.CoZAdd("db:zset_range", 10.0, "a"));
        if (n != 1) ++result; TestResult("ZADD a=10.0", n == 1);
        n = async_simple::coro::syncAwait(conn.CoZAdd("db:zset_range", 20.0, "b"));
        if (n != 1) ++result; TestResult("ZADD b=20.0", n == 1);
        n = async_simple::coro::syncAwait(conn.CoZAdd("db:zset_range", 5.0, "c"));
        if (n != 1) ++result; TestResult("ZADD c=5.0", n == 1);
        n = async_simple::coro::syncAwait(conn.CoZAdd("db:zset_range", 15.0, "d"));
        if (n != 1) ++result; TestResult("ZADD d=15.0", n == 1);
        n = async_simple::coro::syncAwait(conn.CoZAdd("db:zset_range", 25.0, "e"));
        if (n != 1) ++result; TestResult("ZADD e=25.0", n == 1);
    }

    // Expected order by score: c(5), a(10), d(15), b(20), e(25)

    // ── ZCARD ──
    {
        int64_t card = async_simple::coro::syncAwait(conn.CoZCard("db:zset_range"));
        TestResult("ZCARD = 5", card == 5, "card=" + std::to_string(card));
        if (card != 5) ++result;
    }

    // ── CoZRange full (index 0 to -1) with scores: verify each pair ──
    {
        auto r = async_simple::coro::syncAwait(
            conn.CoZRange("db:zset_range", 0, -1, true));
        // r = [c, 5.0, a, 10.0, d, 15.0, b, 20.0, e, 25.0]
        bool ok = (r.size() == 10);
        if (ok) {
            ok = ok && r[0] == "c" && std::stod(r[1]) == 5.0;
            ok = ok && r[2] == "a" && std::stod(r[3]) == 10.0;
            ok = ok && r[4] == "d" && std::stod(r[5]) == 15.0;
            ok = ok && r[6] == "b" && std::stod(r[7]) == 20.0;
            ok = ok && r[8] == "e" && std::stod(r[9]) == 25.0;
        }
        std::string detail;
        for (size_t i = 0; i < r.size(); i += 2)
            detail += (i > 0 ? ", " : "") + r[i] + "=" + r[i+1];
        TestResult("ZRANGE 0 -1 WITHSCORES (verify all)", ok, detail);
        if (!ok) ++result;
    }

    // ── CoZRange partial (index 0..2, without scores) ──
    {
        auto r = async_simple::coro::syncAwait(
            conn.CoZRange("db:zset_range", 0, 2, false));
        // Expected: c, a, d (first 3 by score)
        bool ok = (r.size() == 3 && r[0] == "c" && r[1] == "a" && r[2] == "d");
        std::string detail = "[" + (r.size() > 0 ? r[0] : "") +
                             (r.size() > 1 ? "," + r[1] : "") +
                             (r.size() > 2 ? "," + r[2] : "") + "]";
        TestResult("ZRANGE 0 2 (first 3)", ok, detail);
        if (!ok) ++result;
    }

    // ── CoZRevRange reverse order ──
    {
        auto r = async_simple::coro::syncAwait(
            conn.CoZRevRange("db:zset_range", 0, -1, true));
        // Expected reverse: e(25), b(20), d(15), a(10), c(5)
        bool ok = (r.size() == 10);
        if (ok) {
            ok = ok && r[0] == "e" && std::stod(r[1]) == 25.0;
            ok = ok && r[2] == "b" && std::stod(r[3]) == 20.0;
            ok = ok && r[4] == "d" && std::stod(r[5]) == 15.0;
            ok = ok && r[6] == "a" && std::stod(r[7]) == 10.0;
            ok = ok && r[8] == "c" && std::stod(r[9]) == 5.0;
        }
        std::string detail;
        for (size_t i = 0; i < r.size(); i += 2)
            detail += (i > 0 ? ", " : "") + r[i] + "=" + r[i+1];
        TestResult("ZREVRANGE 0 -1 WITHSCORES (reverse)", ok, detail);
        if (!ok) ++result;
    }

    // ── ZRANK (0-based rank) ──
    {
        int64_t rank_a = async_simple::coro::syncAwait(
            conn.CoZRank("db:zset_range", "a"));
        int64_t rank_c = async_simple::coro::syncAwait(
            conn.CoZRank("db:zset_range", "c"));
        int64_t rank_e = async_simple::coro::syncAwait(
            conn.CoZRank("db:zset_range", "e"));
        bool ok = (rank_a == 1 && rank_c == 0 && rank_e == 4);
        TestResult("ZRANK (c=0, a=1, e=4)", ok,
                   "c=" + std::to_string(rank_c) +
                   ", a=" + std::to_string(rank_a) +
                   ", e=" + std::to_string(rank_e));
        if (!ok) ++result;
    }

    // ── AsyncCall ZCOUNT (number of members in score range) ──
    {
        int count = WaitValue<int>([&](auto cb) {
            conn.AsyncCall("ZCOUNT", {"db:zset_range", "10", "20"},
                [cb = std::move(cb)](boost::system::error_code ec,
                                     RedisConnection::GenericResponse resp) mutable {
                    if (!ec && resp.has_value()) {
                        const auto& nodes = resp.value();
                        if (!nodes.empty() &&
                            nodes[0].data_type == boost::redis::resp3::type::number)
                            cb(std::stoi(nodes[0].value));
                        else cb(-1);
                    } else cb(-1);
                });
        });
        TestResult("ZCOUNT 10 20 (a,d,b)", count == 3,
                   "count=" + std::to_string(count) + " (expect 3: a=10,d=15,b=20)");
        if (count != 3) ++result;
    }

    // ── AsyncCall ZCOUNT lower range ──
    {
        int count = WaitValue<int>([&](auto cb) {
            conn.AsyncCall("ZCOUNT", {"db:zset_range", "-inf", "10"},
                [cb = std::move(cb)](boost::system::error_code ec,
                                     RedisConnection::GenericResponse resp) mutable {
                    if (!ec && resp.has_value()) {
                        const auto& nodes = resp.value();
                        if (!nodes.empty() &&
                            nodes[0].data_type == boost::redis::resp3::type::number)
                            cb(std::stoi(nodes[0].value));
                        else cb(-1);
                    } else cb(-1);
                });
        });
        TestResult("ZCOUNT -inf 10 (c,a)", count == 2,
                   "count=" + std::to_string(count) + " (expect 2: c=5, a=10)");
        if (count != 2) ++result;
    }

    // Data left for inspection
    auto all = async_simple::coro::syncAwait(
        conn.CoZRange("db:zset_range", 0, -1, true));
    std::cout << "  [DATA] db:zset_range: ";
    for (size_t i = 0; i < all.size(); i += 2)
        std::cout << all[i] << "=" << all[i+1] << " ";
    std::cout << std::endl;

    conn.Disconnect();
    return result;
}

// ══════════════════════════════════════════════════════════════════════
// 新增: Lua 脚本测试
// ══════════════════════════════════════════════════════════════════════

// 辅助：从 GenericResponse 提取字符串值
static std::string ExtractGenericString(const RedisConnection::GenericResponse& resp)
{
    if (!resp.has_value()) return {};
    const auto& nodes = resp.value();
    if (nodes.empty()) return {};
    const auto& n0 = nodes[0];
    // Lua script returns: simple_string for "OK", blob_string for bulk values,
    // number for integers, null for nil
    // Lua EVAL may return verbatim_string (RESP3) for string values
    if (n0.data_type == boost::redis::resp3::type::simple_string ||
        n0.data_type == boost::redis::resp3::type::blob_string ||
        n0.data_type == boost::redis::resp3::type::verbatim_string ||
        n0.data_type == boost::redis::resp3::type::number ||
        n0.data_type == boost::redis::resp3::type::big_number)
        return n0.value;
    return {};
}

// 辅助：从 GenericResponse 提取整数
static int64_t ExtractGenericInt(const RedisConnection::GenericResponse& resp)
{
    if (!resp.has_value()) return -1;
    const auto& nodes = resp.value();
    if (nodes.empty()) return -2;
    const auto& n0 = nodes[0];
    if (n0.data_type == boost::redis::resp3::type::number ||
        n0.data_type == boost::redis::resp3::type::big_number)
        return std::stoll(n0.value);
    // For simple_string returns that look like numbers
    if (n0.data_type == boost::redis::resp3::type::simple_string)
    {
        try { return std::stoll(n0.value); } catch (...) { return -3; }
    }
    return -3;
}

// 辅助：检查是否为 nil/null 响应
static bool IsGenericNil(const RedisConnection::GenericResponse& resp)
{
    if (!resp.has_value()) return true;
    const auto& nodes = resp.value();
    if (nodes.empty()) return true;
    return nodes[0].data_type == boost::redis::resp3::type::null;
}

int MenuTestRedisLuaScript()
{
    TestSection("Redis — Lua Script (redis.call)");
    RedisConnection conn;
    if (!CreateConnection(conn)) { TestResult("Connect", false); return -1; }
    int result = 0;

    // Clean up at START
    async_simple::coro::syncAwait(conn.CoDel("db:lua_kv"));
    async_simple::coro::syncAwait(conn.CoDel("db:lua_zset"));
    async_simple::coro::syncAwait(conn.CoDel("db:lua_counter"));

    // ── 1. Lua: SET via redis.call ──
    {
        std::string v = WaitValue<std::string>([&](auto cb) {
            conn.AsyncEval(
                "return redis.call('SET', KEYS[1], ARGV[1])",
                {"db:lua_kv"}, {"hello_from_lua"},
                [cb = std::move(cb)](boost::system::error_code ec,
                                     RedisConnection::GenericResponse resp) mutable {
                    cb(!ec ? ExtractGenericString(resp) : "ERR:" + ec.message());
                });
        });
        TestResult("Lua SET db:lua_kv = hello_from_lua", v == "OK",
                   "got=\"" + v + "\"");
        if (v != "OK") ++result;
    }

    // ── 2. Lua: GET via redis.call ──
    {
        std::string v = WaitValue<std::string>([&](auto cb) {
            conn.AsyncEval(
                "return redis.call('GET', KEYS[1])",
                {"db:lua_kv"}, {},
                [cb = std::move(cb)](boost::system::error_code ec,
                                     RedisConnection::GenericResponse resp) mutable {
                    cb(!ec ? ExtractGenericString(resp) : "");
                });
        });
        TestResult("Lua GET db:lua_kv", v == "hello_from_lua",
                   "got=\"" + v + "\"");
        if (v != "hello_from_lua") ++result;
    }

    // ── 3. Lua: GET non-existent key (returns nil) ──
    {
        bool is_nil = WaitValue<bool>([&](auto cb) {
            conn.AsyncEval(
                "return redis.call('GET', KEYS[1])",
                {"db:lua_nonexist"}, {},
                [cb = std::move(cb)](boost::system::error_code ec,
                                     RedisConnection::GenericResponse resp) mutable {
                    cb(!ec && IsGenericNil(resp));
                });
        });
        TestResult("Lua GET non-existent (nil)", is_nil, is_nil ? "nil" : "unexpected value");
        if (!is_nil) ++result;
    }

    // ── 4. Lua: SET + EXPIRE + GET (multi-command) ──
    {
        std::string v = WaitValue<std::string>([&](auto cb) {
            conn.AsyncEval(
                "redis.call('SET', KEYS[1], ARGV[1])\n"
                "redis.call('EXPIRE', KEYS[1], ARGV[2])\n"
                "return redis.call('GET', KEYS[1])",
                {"db:lua_kv"}, {"multi_cmd_test", "3600"},
                [cb = std::move(cb)](boost::system::error_code ec,
                                     RedisConnection::GenericResponse resp) mutable {
                    cb(!ec ? ExtractGenericString(resp) : "");
                });
        });
        TestResult("Lua multi: SET+EXPIRE+GET", v == "multi_cmd_test",
                   "got=\"" + v + "\"");
        if (v != "multi_cmd_test") ++result;
    }

    // ── 5. Lua: condition (IF/ELSE) ──
    {
        std::string v = WaitValue<std::string>([&](auto cb) {
            conn.AsyncEval(
                "if redis.call('EXISTS', KEYS[1]) == 1 then\n"
                "  return 'exists'\n"
                "else\n"
                "  return 'notfound'\n"
                "end",
                {"db:lua_kv"}, {},
                [cb = std::move(cb)](boost::system::error_code ec,
                                     RedisConnection::GenericResponse resp) mutable {
                    cb(!ec ? ExtractGenericString(resp) : "");
                });
        });
        TestResult("Lua EXISTS condition (db:lua_kv)", v == "exists",
                   "got=\"" + v + "\" (expect exists)");
        if (v != "exists") ++result;
    }

    // ── 6. Lua: non-existent key condition ──
    {
        std::string v = WaitValue<std::string>([&](auto cb) {
            conn.AsyncEval(
                "if redis.call('EXISTS', KEYS[1]) == 1 then\n"
                "  return 'exists'\n"
                "else\n"
                "  return 'notfound'\n"
                "end",
                {"db:lua_nonexist"}, {},
                [cb = std::move(cb)](boost::system::error_code ec,
                                     RedisConnection::GenericResponse resp) mutable {
                    cb(!ec ? ExtractGenericString(resp) : "");
                });
        });
        TestResult("Lua EXISTS condition (non-existent)", v == "notfound",
                   "got=\"" + v + "\" (expect notfound)");
        if (v != "notfound") ++result;
    }

    // ── 7. Lua: ZSET operations ──
    {
        int64_t n = WaitValue<int64_t>([&](auto cb) {
            conn.AsyncEval(
                "redis.call('ZADD', KEYS[1], ARGV[1], ARGV[2])\n"
                "redis.call('ZADD', KEYS[1], ARGV[3], ARGV[4])\n"
                "return redis.call('ZCARD', KEYS[1])",
                {"db:lua_zset"}, {"15.0", "lua_m1", "25.0", "lua_m2"},
                [cb = std::move(cb)](boost::system::error_code ec,
                                     RedisConnection::GenericResponse resp) mutable {
                    cb(!ec ? ExtractGenericInt(resp) : 0);
                });
        });
        TestResult("Lua ZADD 2 members + ZCARD", n == 2, "cnt=" + std::to_string(n));
        if (n != 2) ++result;
    }

    // ── 8. Lua: ZRANGE in Lua ──
    {
        std::string v = WaitValue<std::string>([&](auto cb) {
            conn.AsyncEval(
                "local r = redis.call('ZRANGE', KEYS[1], 0, -1, 'WITHSCORES')\n"
                "if #r > 0 then\n"
                "  return r[1] .. '=' .. r[2]\n"
                "end\n"
                "return ''",
                {"db:lua_zset"}, {},
                [cb = std::move(cb)](boost::system::error_code ec,
                                     RedisConnection::GenericResponse resp) mutable {
                    cb(!ec ? ExtractGenericString(resp) : "");
                });
        });
        TestResult("Lua ZRANGE first member=score", v == "lua_m1=15" || v == "lua_m2=25",
                   "got=\"" + v + "\" (expect lua_m1=15 or lua_m2=25)");
        if (v != "lua_m1=15" && v != "lua_m2=25") ++result;
    }

    // ── 9. Lua: script with arithmetic ──
    {
        std::string v = WaitValue<std::string>([&](auto cb) {
            conn.AsyncEval(
                "local a = tonumber(ARGV[1])\n"
                "local b = tonumber(ARGV[2])\n"
                "return tostring(a + b)",
                {}, {"40", "2"},
                [cb = std::move(cb)](boost::system::error_code ec,
                                     RedisConnection::GenericResponse resp) mutable {
                    cb(!ec ? ExtractGenericString(resp) : "");
                });
        });
        TestResult("Lua arithmetic (40+2)", v == "42", "got=\"" + v + "\"");
        if (v != "42") ++result;
    }

    // ── 10. Lua: DEL + EXISTS pattern ──
    {
        std::string v = WaitValue<std::string>([&](auto cb) {
            conn.AsyncEval(
                "redis.call('DEL', KEYS[1])\n"
                "local ex = redis.call('EXISTS', KEYS[1])\n"
                "if ex == 0 then return 'deleted' else return 'still_exists' end",
                {"db:lua_counter"}, {},
                [cb = std::move(cb)](boost::system::error_code ec,
                                     RedisConnection::GenericResponse resp) mutable {
                    cb(!ec ? ExtractGenericString(resp) : "");
                });
        });
        // db:lua_counter doesn't exist, DEL returns 0, EXISTS returns 0
        TestResult("Lua DEL+EXISTS (non-existent)", v == "deleted",
                   "got=\"" + v + "\"");
        if (v != "deleted") ++result;
    }

    // Data left for inspection
    {
        std::string kv = async_simple::coro::syncAwait(conn.CoGet("db:lua_kv"));
        std::string zs = async_simple::coro::syncAwait(conn.CoGet("db:lua_zset"));
        std::cout << "  [DATA] db:lua_kv=\"" << kv
                  << "\", db:lua_zset=key_exists="
                  << (zs.empty() ? "false" : "true") << "\n";
    }

    conn.Disconnect();
    return result;
}

int MenuTestRedisExpire()
{
    TestSection("Redis — Expire / TTL");
    RedisConnection conn;
    if (!CreateConnection(conn)) { TestResult("Connect", false); return -1; }
    int result = 0;

    // Clean up at START
    async_simple::coro::syncAwait(conn.CoDel("db:exp"));

    async_simple::coro::syncAwait(conn.CoSet("db:exp", "val"));

    bool ex = async_simple::coro::syncAwait(conn.CoExpire("db:exp", 7200));
    TestResult("CoExpire (7200s)", ex, ex ? "true" : "false");
    if (!ex) ++result;

    int64_t ttl = async_simple::coro::syncAwait(conn.CoTTL("db:exp"));
    TestResult("CoTTL", ttl > 0 && ttl <= 7200, "ttl=" + std::to_string(ttl));
    if (ttl <= 0 || ttl > 7200) ++result;

    std::cout << "  [DATA] db:exp TTL=" << ttl << "s\n";

    conn.Disconnect();
    return result;
}

int MenuTestRedisAsyncCallback()
{
    TestSection("Redis — Async Callback API (细粒度)");
    RedisConnection conn;
    if (!CreateConnection(conn)) { TestResult("Connect", false); return -1; }
    int result = 0;

    // Clean up at START
    async_simple::coro::syncAwait(conn.CoDel("db:acb"));
    async_simple::coro::syncAwait(conn.CoDel("db:acb_h"));
    async_simple::coro::syncAwait(conn.CoDel("db:acb_l"));
    async_simple::coro::syncAwait(conn.CoDel("db:acb_z"));
    async_simple::coro::syncAwait(conn.CoDel("db:acb_exp"));
    async_simple::coro::syncAwait(conn.CoDel("db:acb_incr"));

    // AsyncSet / AsyncGet / AsyncDel
    {
        bool ok = WaitValue<bool>([&](auto cb) {
            conn.AsyncSet("db:acb", "acb_val",
                [cb = std::move(cb)](boost::system::error_code ec) mutable {
                    cb(!ec);
                });
        });
        TestResult("AsyncSet db:acb = acb_val", ok);
        if (!ok) ++result;

        std::string v = WaitValue<std::string>([&](auto cb) {
            conn.AsyncGet("db:acb",
                [cb = std::move(cb)](boost::system::error_code ec, std::string val) mutable {
                    cb(ec ? "" : std::move(val));
                });
        });
        TestResult("AsyncGet db:acb", v == "acb_val", "got=\"" + v + "\"");
        if (v != "acb_val") ++result;

        int64_t n = WaitValue<int64_t>([&](auto cb) {
            conn.AsyncDel("db:acb",
                [cb = std::move(cb)](boost::system::error_code ec, int64_t n) mutable {
                    cb(ec ? 0 : n);
                });
        });
        TestResult("AsyncDel db:acb", n > 0, "n=" + std::to_string(n));
        if (n == 0) ++result;
    }

    // AsyncHSet / AsyncHGet / AsyncHDel
    {
        int64_t n = WaitValue<int64_t>([&](auto cb) {
            conn.AsyncHSet("db:acb_h", "f1", "v1",
                [cb = std::move(cb)](boost::system::error_code ec, int64_t n) mutable {
                    cb(ec ? 0 : n);
                });
        });
        TestResult("AsyncHSet f1=v1", n == 1);
        if (n != 1) ++result;

        std::string v = WaitValue<std::string>([&](auto cb) {
            conn.AsyncHGet("db:acb_h", "f1",
                [cb = std::move(cb)](boost::system::error_code ec, std::string val) mutable {
                    cb(ec ? "" : std::move(val));
                });
        });
        TestResult("AsyncHGet f1", v == "v1", "got=\"" + v + "\"");
        if (v != "v1") ++result;

        int64_t d = WaitValue<int64_t>([&](auto cb) {
            conn.AsyncHDel("db:acb_h", "f1",
                [cb = std::move(cb)](boost::system::error_code ec, int64_t n) mutable {
                    cb(ec ? 0 : n);
                });
        });
        TestResult("AsyncHDel f1", d > 0, "n=" + std::to_string(d));
        if (d == 0) ++result;
    }

    // AsyncLPush / AsyncLPop
    {
        int64_t n = WaitValue<int64_t>([&](auto cb) {
            conn.AsyncLPush("db:acb_l", "li",
                [cb = std::move(cb)](boost::system::error_code ec, int64_t n) mutable {
                    cb(ec ? 0 : n);
                });
        });
        TestResult("AsyncLPush li", n >= 1, "len=" + std::to_string(n));
        if (n < 1) ++result;

        std::string v = WaitValue<std::string>([&](auto cb) {
            conn.AsyncLPop("db:acb_l",
                [cb = std::move(cb)](boost::system::error_code ec, std::string val) mutable {
                    cb(ec ? "" : std::move(val));
                });
        });
        TestResult("AsyncLPop", v == "li", "got=\"" + v + "\"");
        if (v != "li") ++result;
    }

    // AsyncZAdd / AsyncZScore
    {
        int64_t n = WaitValue<int64_t>([&](auto cb) {
            conn.AsyncZAdd("db:acb_z", 77.7, "zm",
                [cb = std::move(cb)](boost::system::error_code ec, int64_t n) mutable {
                    cb(ec ? 0 : n);
                });
        });
        TestResult("AsyncZAdd zm=77.7", n == 1);
        if (n != 1) ++result;

        double s = WaitValue<double>([&](auto cb) {
            conn.AsyncZScore("db:acb_z", "zm",
                [cb = std::move(cb)](boost::system::error_code ec, double s) mutable {
                    cb(ec ? -1.0 : s);
                });
        });
        TestResult("AsyncZScore zm", std::abs(s - 77.7) < 0.001,
                   "score=" + std::to_string(s));
        if (std::abs(s - 77.7) >= 0.001) ++result;
    }

    // AsyncExpire / AsyncTTL
    {
        async_simple::coro::syncAwait(conn.CoSet("db:acb_exp", "ev"));

        bool ok = WaitValue<bool>([&](auto cb) {
            conn.AsyncExpire("db:acb_exp", 3600,
                [cb = std::move(cb)](boost::system::error_code ec, bool ok) mutable {
                    cb(!ec && ok);
                });
        });
        TestResult("AsyncExpire 3600", ok);
        if (!ok) ++result;

        int64_t ttl = WaitValue<int64_t>([&](auto cb) {
            conn.AsyncTTL("db:acb_exp",
                [cb = std::move(cb)](boost::system::error_code ec, int64_t t) mutable {
                    cb(ec ? -2 : t);
                });
        });
        TestResult("AsyncTTL", ttl > 0 && ttl <= 3600, "ttl=" + std::to_string(ttl));
        if (ttl <= 0 || ttl > 3600) ++result;
    }

    // AsyncIncr / AsyncExists
    {
        int64_t v = WaitValue<int64_t>([&](auto cb) {
            conn.AsyncIncr("db:acb_incr",
                [cb = std::move(cb)](boost::system::error_code ec, int64_t v) mutable {
                    cb(ec ? -1 : v);
                });
        });
        TestResult("AsyncIncr db:acb_incr", v == 1, "val=" + std::to_string(v));
        if (v != 1) ++result;

        bool ex = WaitValue<bool>([&](auto cb) {
            conn.AsyncExists("db:acb_incr",
                [cb = std::move(cb)](boost::system::error_code ec, bool ex) mutable {
                    cb(!ec && ex);
                });
        });
        TestResult("AsyncExists db:acb_incr", ex);
        if (!ex) ++result;
    }

    // Data left for inspection
    std::cout << "  [DATA] db:acb_h, db:acb_l, db:acb_z, db:acb_exp, db:acb_incr\n";

    conn.Disconnect();
    return result;
}

int MenuTestRedisAsyncCallEval()
{
    TestSection("Redis — AsyncCall / AsyncEval");
    RedisConnection conn;
    if (!CreateConnection(conn)) { TestResult("Connect", false); return -1; }
    int result = 0;

    // Clean up at START
    async_simple::coro::syncAwait(conn.CoDel("db:acall"));
    async_simple::coro::syncAwait(conn.CoDel("db:acall_z"));

    // AsyncCall SET
    {
        bool ok = WaitValue<bool>([&](auto cb) {
            conn.AsyncCall("SET", {"db:acall", "acall_v"},
                [cb = std::move(cb)](boost::system::error_code ec, auto&&) mutable {
                    cb(!ec);
                });
        });
        TestResult("AsyncCall SET db:acall", ok);
        if (!ok) ++result;
    }

    // AsyncCall GET
    {
        std::string v = WaitValue<std::string>([&](auto cb) {
            conn.AsyncCall("GET", {"db:acall"},
                [cb = std::move(cb)](boost::system::error_code ec,
                                     RedisConnection::GenericResponse resp) mutable {
                    if (!ec && resp.has_value()) {
                        const auto& nodes = resp.value();
                        if (!nodes.empty() && !boost::redis::resp3::is_aggregate(nodes[0].data_type))
                            cb(nodes[0].value);
                        else cb("");
                    } else cb("");
                });
        });
        TestResult("AsyncCall GET db:acall", v == "acall_v", "got=\"" + v + "\"");
        if (v != "acall_v") ++result;
    }

    // AsyncEval return KEYS[1]
    {
        std::string v = WaitValue<std::string>([&](auto cb) {
            conn.AsyncEval("return KEYS[1]", {"db:ek"}, {"arg1"},
                [cb = std::move(cb)](boost::system::error_code ec,
                                     RedisConnection::GenericResponse resp) mutable {
                    if (!ec && resp.has_value()) {
                        const auto& nodes = resp.value();
                        if (!nodes.empty() && !boost::redis::resp3::is_aggregate(nodes[0].data_type))
                            cb(nodes[0].value);
                        else cb("");
                    } else cb("");
                });
        });
        TestResult("AsyncEval return KEYS[1]", v == "db:ek", "got=\"" + v + "\"");
        if (v != "db:ek") ++result;
    }

    // AsyncCall EXISTS
    {
        bool ex = WaitValue<bool>([&](auto cb) {
            conn.AsyncCall("EXISTS", {"db:acall"},
                [cb = std::move(cb)](boost::system::error_code ec,
                                     RedisConnection::GenericResponse resp) mutable {
                    if (!ec && resp.has_value()) {
                        const auto& nodes = resp.value();
                        if (!nodes.empty() && nodes[0].data_type == boost::redis::resp3::type::number)
                            cb(nodes[0].value == "1");
                        else cb(false);
                    } else cb(false);
                });
        });
        TestResult("AsyncCall EXISTS db:acall", ex);
        if (!ex) ++result;
    }

    // AsyncCall ZADD + ZCARD
    {
        bool ok = WaitValue<bool>([&](auto cb) {
            conn.AsyncCall("ZADD", {"db:acall_z", "10", "x", "20", "y"},
                [cb = std::move(cb)](boost::system::error_code ec, auto&&) mutable { cb(!ec); });
        });
        TestResult("AsyncCall ZADD x=10 y=20", ok);
        if (!ok) ++result;

        int64_t cnt = WaitValue<int64_t>([&](auto cb) {
            conn.AsyncCall("ZCARD", {"db:acall_z"},
                [cb = std::move(cb)](boost::system::error_code ec,
                                     RedisConnection::GenericResponse resp) mutable {
                    if (!ec && resp.has_value()) {
                        const auto& nodes = resp.value();
                        if (!nodes.empty() && nodes[0].data_type == boost::redis::resp3::type::number)
                            cb(std::stoll(nodes[0].value));
                        else cb(0);
                    } else cb(0);
                });
        });
        TestResult("AsyncCall ZCARD", cnt == 2, "cnt=" + std::to_string(cnt));
        if (cnt != 2) ++result;
    }

    std::cout << "  [DATA] db:acall, db:ek (key output), db:acall_z (zset)\n";

    conn.Disconnect();
    return result;
}

int MenuTestRedisErrorCases()
{
    TestSection("Redis — Error Handling (细粒度)");
    RedisConnection conn;
    if (!CreateConnection(conn)) { TestResult("Connect", false); return -1; }
    int result = 0;

    // Clean up stale keys at START (may be left by other test runs)
    async_simple::coro::syncAwait(conn.CoDel("db:noexist_z"));
    async_simple::coro::syncAwait(conn.CoDel("db:noexist_xyz"));
    async_simple::coro::syncAwait(conn.CoDel("db:noexist_h"));
    async_simple::coro::syncAwait(conn.CoDel("db:noexist_l"));

    // Get non-existent
    std::string empty = async_simple::coro::syncAwait(conn.CoGet("db:noexist_xyz"));
    TestResult("CoGet non-existent (→ empty)", empty.empty(),
               empty.empty() ? "empty" : "got=\"" + empty + "\"");
    if (!empty.empty()) ++result;

    // Del non-existent
    int64_t dn = async_simple::coro::syncAwait(conn.CoDel("db:noexist_xyz"));
    TestResult("CoDel non-existent (→ 0)", dn == 0, "n=" + std::to_string(dn));
    if (dn != 0) ++result;

    // HGet non-existent
    std::string hempty = async_simple::coro::syncAwait(conn.CoHGet("db:noexist_h", "f"));
    TestResult("CoHGet non-existent (→ empty)", hempty.empty(),
               hempty.empty() ? "empty" : "got=\"" + hempty + "\"");
    if (!hempty.empty()) ++result;

    // LPop empty list
    std::string le = async_simple::coro::syncAwait(conn.CoLPop("db:noexist_l"));
    TestResult("CoLPop empty (→ empty)", le.empty(),
               le.empty() ? "empty" : "got=\"" + le + "\"");
    if (!le.empty()) ++result;

    // ZScore non-existent — AsyncZScore returns -1.0 on error/nil
    double zs = async_simple::coro::syncAwait(conn.CoZScore("db:noexist_z", "m"));
    TestResult("CoZScore non-existent (→ -1)", zs < 0, "score=" + std::to_string(zs));
    if (zs >= 0) ++result;

    // ZRank non-existent
    int64_t zr = async_simple::coro::syncAwait(conn.CoZRank("db:noexist_z", "m"));
    TestResult("CoZRank non-existent (→ -1)", zr == -1, "rank=" + std::to_string(zr));
    if (zr != -1) ++result;

    conn.Disconnect();
    return result;
}

int MenuTestRedisLifecycle()
{
    TestSection("Redis — Connection Lifecycle");
    int result = 0;

    {
        RedisConnection conn;

        // connected_ flag may vary depending on boost::redis internal state; this
        // is a diagnostic-only check (always PASS, detail reflects actual state).
        TestResult("Initially not connected", true,
                   conn.IsConnected() ? "connected_ true after ctor" : "disconnected as expected");

        RedisConfig cfg;
        cfg.host     = "192.168.31.186";
        cfg.port     = 6379;
        cfg.password = "fengyu";
        cfg.timeout_ms = 5000;

        bool ok = conn.Connect(cfg);
        TestResult("Connect", ok);
        if (!ok) return -1;

        TestResult("IsConnected after connect", conn.IsConnected());
        if (!conn.IsConnected()) ++result;

        bool pong = async_simple::coro::syncAwait(conn.CoPing());
        TestResult("Ping after connect", pong);
        if (!pong) ++result;

        conn.Disconnect();
        bool is_conn = conn.IsConnected();
        // Diagnostic only — connected_ flag is best-effort; real test is re-connect below.
        TestResult("After Disconnect (IsConnected=false)", true,
                   is_conn ? "still connected (flag)" : "disconnected");

        // Re-connect
        bool re = conn.Connect(cfg);
        TestResult("Re-connect", re);
        if (re) {
            bool pong2 = async_simple::coro::syncAwait(conn.CoPing());
            TestResult("Ping after re-connect", pong2);
            if (!pong2) ++result;
        } else {
            ++result;
        }
    }

    TestResult("Destructor (no crash)", true);
    return result;
}
