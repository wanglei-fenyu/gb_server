#include "pg_test.h"
#include "db/postgres/pg_connection.h"
#include "db/postgres/db_config.h"
#include "db/postgres/db_result.h"
#include "report.h"
#include "async_simple/Future.h"
#include "async_simple/Promise.h"
#include "async_simple/coro/SyncAwait.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <thread>

// ── 默认 PG 配置 ──
static gb::DbConfig DefaultPgConfig()
{
    gb::DbConfig cfg;
    cfg.host     = "192.168.31.186";
    cfg.port     = 5432;
    cfg.user     = "fys";
    cfg.password = "fengyu";
    cfg.database = "mydb";
    cfg.use_ssl  = false;
    return cfg;
}

// ── 创建一个 io_context + 线程环境 ──
struct IoEnv {
    boost::asio::io_context                                          io_ctx;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard;
    std::thread                                                      thread;

    IoEnv() : guard(boost::asio::make_work_guard(io_ctx))
            , thread([this]() { io_ctx.run(); }) {}

    ~IoEnv() { guard.reset(); if (thread.joinable()) thread.join(); }
};

// ── Promise/Future 桥接辅助 ──
template <typename T>
static T WaitValue(const std::function<void(std::function<void(T)>)>& setup)
{
    async_simple::Promise<T> p;
    auto f = p.getFuture();
    setup([p = std::move(p)](T val) mutable { p.setValue(std::move(val)); });
    return std::move(f).get();
}

static void WaitDone(const std::function<void(std::function<void()>)>& setup)
{
    async_simple::Promise<void> p;
    auto f = p.getFuture();
    setup([p = std::move(p)]() mutable { p.setValue(); });
    std::move(f).get();
}

// ══════════════════════════════════════════════════════════════════════
// 测试用例
// ══════════════════════════════════════════════════════════════════════

int MenuTestPgConnectClose()
{
    TestSection("PG — AsyncConnect / AsyncClose");
    IoEnv env;
    int result = 0;

    auto conn = std::make_shared<gb::PgConnection>(env.io_ctx);
    gb::DbConfig cfg = DefaultPgConfig();

    // Connect
    bool ok = WaitValue<bool>([&](auto cb) {
        conn->AsyncConnect(cfg, [cb = std::move(cb)](bool ok) mutable { cb(ok); });
    });
    TestResult("AsyncConnect", ok, ok ? "ok" : "failed");
    if (!ok) { ++result; }

    TestResult("IsConnected after connect", conn->IsConnected());

    // Close
    WaitDone([&](auto cb) {
        conn->AsyncClose([cb = std::move(cb)]() mutable { cb(); });
    });
    TestResult("AsyncClose (IsConnected=false)", !conn->IsConnected());

    conn.reset();
    return result;
}

int MenuTestPgQuery()
{
    TestSection("PG — AsyncQuery (细粒度)");
    IoEnv env;
    int result = 0;

    auto conn = std::make_shared<gb::PgConnection>(env.io_ctx);
    gb::DbConfig cfg = DefaultPgConfig();

    // ── Connect ──
    bool ok = WaitValue<bool>([&](auto cb) {
        conn->AsyncConnect(cfg, [cb = std::move(cb)](bool ok) mutable { cb(ok); });
    });
    if (!ok) { TestResult("Connect", false); return -1; }
    TestResult("Connect", true);

    // ── Clean up old test data at START (not end) ──
    {
        WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery("DELETE FROM db_test_users",
                [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
    }

    // ── SELECT literal ──
    {
        bool q_ok = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery("SELECT 42 AS num, 'db_test' AS txt",
                [cb = std::move(cb)](gb::DbResult res) mutable {
                    bool ok = res.is_ok() && res.rows_count() > 0;
                    if (ok) {
                        auto row = const_cast<gb::DbResult&>(res).next();
                        ok = (row[0].as_int32() == 42 &&
                              std::string(row[1].as_string()) == "db_test");
                    }
                    cb(ok);
                });
        });
        TestResult("SELECT literal 42", q_ok, "num=42, txt=db_test");
        if (!q_ok) ++result;
    }

    // ── CREATE TABLE (stay after test) ──
    {
        bool ct_ok = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                CREATE TABLE IF NOT EXISTS db_test_users (
                    id      SERIAL PRIMARY KEY,
                    name    VARCHAR(100) NOT NULL,
                    email   VARCHAR(100),
                    score   INTEGER DEFAULT 0
                )
            )SQL", [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        TestResult("CREATE TABLE db_test_users", ct_ok);
        if (!ct_ok) ++result;
    }

    // ── Parameterized INSERT ──
    {
        bool ins_ok = WaitValue<bool>([&](auto cb) {
            std::vector<gb::DbValue> params = {
                gb::DbValue("alice"),
                gb::DbValue("alice@test.com"),
                gb::DbValue(100)
            };
            conn->AsyncQuery(
                "INSERT INTO db_test_users (name, email, score) "
                "VALUES ($1, $2, $3) RETURNING id",
                params,
                [cb = std::move(cb)](gb::DbResult res) mutable {
                    bool ok = res.is_ok() && res.rows_count() > 0;
                    if (ok) {
                        auto row = const_cast<gb::DbResult&>(res).next();
                        ok = row[0].as_int32() > 0;  // auto-generated id
                        if (ok)
                            std::cout << "  [DATA] INSERT id=" << row[0].as_int32() << std::endl;
                    }
                    cb(ok);
                });
        });
        TestResult("INSERT alice (100)", ins_ok);
        if (!ins_ok) ++result;
    }

    // ── Parameterized SELECT (verify inserted data) ──
    {
        bool sel_ok = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(
                "SELECT name, score FROM db_test_users WHERE email = $1",
                std::vector<gb::DbValue>{gb::DbValue("alice@test.com")},
                [cb = std::move(cb)](gb::DbResult res) mutable {
                    bool ok = false;
                    if (res.is_ok() && res.rows_count() > 0) {
                        auto row = const_cast<gb::DbResult&>(res).next();
                        ok = (std::string(row[0].as_string()) == "alice" &&
                              row[1].as_int32() == 100);
                        if (ok)
                            std::cout << "  [DATA] SELECT alice score="
                                      << row[1].as_int32() << std::endl;
                    }
                    cb(ok);
                });
        });
        TestResult("SELECT verify alice (score=100)", sel_ok);
        if (!sel_ok) ++result;
    }

    // ── Insert second user ──
    {
        bool ins_ok = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(
                "INSERT INTO db_test_users (name, email, score) VALUES ($1, $2, $3)",
                std::vector<gb::DbValue>{gb::DbValue("bob"), gb::DbValue("bob@test.com"), gb::DbValue(200)},
                [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        TestResult("INSERT bob (200)", ins_ok);
        if (!ins_ok) ++result;
    }

    // ── SELECT all ──
    {
        int row_count = WaitValue<int>([&](auto cb) {
            conn->AsyncQuery("SELECT COUNT(*) AS cnt FROM db_test_users",
                [cb = std::move(cb)](gb::DbResult res) mutable {
                    int cnt = 0;
                    if (res.is_ok() && res.rows_count() > 0) {
                        auto row = const_cast<gb::DbResult&>(res).next();
                        cnt = row[0].as_int32();
                    }
                    cb(cnt);
                });
        });
        TestResult("SELECT COUNT(*)", row_count == 2,
                   "rows=" + std::to_string(row_count));
        if (row_count != 2) ++result;
    }

    // NO DROP — data stays for inspection
    WaitDone([&](auto cb) {
        conn->AsyncClose([cb = std::move(cb)]() mutable { cb(); });
    });
    conn.reset();
    return result;
}

int MenuTestPgExecute()
{
    TestSection("PG — AsyncExecute (细粒度)");
    IoEnv env;
    int result = 0;

    auto conn = std::make_shared<gb::PgConnection>(env.io_ctx);
    gb::DbConfig cfg = DefaultPgConfig();

    // ── Connect ──
    bool ok = WaitValue<bool>([&](auto cb) {
        conn->AsyncConnect(cfg, [cb = std::move(cb)](bool ok) mutable { cb(ok); });
    });
    if (!ok) { TestResult("Connect", false); return -1; }
    TestResult("Connect", true);

    // ── Clean up old data at START ──
    {
        WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery("DROP TABLE IF EXISTS db_test_exec",
                [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
    }

    // ── CREATE TABLE ──
    {
        bool ct = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                CREATE TABLE db_test_exec (
                    id   SERIAL PRIMARY KEY,
                    val  INTEGER
                )
            )SQL", [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        TestResult("CREATE TABLE db_test_exec", ct);
        if (!ct) ++result;
    }

    // ── INSERT via Query ──
    {
        std::vector<gb::DbValue> params = {gb::DbValue(1), gb::DbValue(2), gb::DbValue(3)};
        bool ins = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery("INSERT INTO db_test_exec (val) VALUES ($1),($2),($3)",
                params,
                [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        TestResult("INSERT 3 rows (1,2,3)", ins);
        if (!ins) ++result;
    }

    // ── Verify count after INSERT ──
    {
        int cnt = WaitValue<int>([&](auto cb) {
            conn->AsyncQuery("SELECT COUNT(*) FROM db_test_exec",
                [cb = std::move(cb)](gb::DbResult res) mutable {
                    int c = 0;
                    if (res.is_ok() && res.rows_count() > 0)
                        c = const_cast<gb::DbResult&>(res).next()[0].as_int32();
                    cb(c);
                });
        });
        TestResult("COUNT after INSERT", cnt == 3, "cnt=" + std::to_string(cnt));
        if (cnt != 3) ++result;
    }

    // ── UPDATE via AsyncQuery ──
    {
        uint64_t n = WaitValue<uint64_t>([&](auto cb) {
            conn->AsyncQuery("UPDATE db_test_exec SET val = val + 10 WHERE val = $1",
                std::vector<gb::DbValue>{gb::DbValue(1)},
                [cb = std::move(cb)](gb::DbResult res) mutable {
                    cb(res.is_ok() ? res.affected_rows() : 0);
                });
        });
        TestResult("UPDATE val+10 WHERE val=1", n > 0, "n=" + std::to_string(n));
        if (n == 0) ++result;
    }

    // ── Verify updated value ──
    {
        int v = WaitValue<int>([&](auto cb) {
            conn->AsyncQuery("SELECT val FROM db_test_exec WHERE val = 11",
                [cb = std::move(cb)](gb::DbResult res) mutable {
                    int val = 0;
                    if (res.is_ok() && res.rows_count() > 0)
                        val = const_cast<gb::DbResult&>(res).next()[0].as_int32();
                    cb(val);
                });
        });
        TestResult("Verify val=11 after UPDATE", v == 11, "val=" + std::to_string(v));
        if (v != 11) ++result;
    }

    // ── AsyncExecute DELETE ──
    {
        uint64_t n = WaitValue<uint64_t>([&](auto cb) {
            conn->AsyncExecute("DELETE FROM db_test_exec WHERE val < 10",
                [cb = std::move(cb)](uint64_t n) mutable { cb(n); });
        });
        TestResult("AsyncExecute DELETE (rows<20)", n > 0, "n=" + std::to_string(n));
        if (n == 0) ++result;
    }

    // ── Verify remaining ──
    {
        int rem = WaitValue<int>([&](auto cb) {
            conn->AsyncQuery("SELECT COUNT(*) FROM db_test_exec",
                [cb = std::move(cb)](gb::DbResult res) mutable {
                    int c = 0;
                    if (res.is_ok() && res.rows_count() > 0)
                        c = const_cast<gb::DbResult&>(res).next()[0].as_int32();
                    cb(c);
                });
        });
        TestResult("Remaining rows after DELETE", rem == 1, "rem=" + std::to_string(rem));
        if (rem != 1) ++result;
    }

    // NO DROP — data stays for inspection
    WaitDone([&](auto cb) {
        conn->AsyncClose([cb = std::move(cb)]() mutable { cb(); });
    });
    conn.reset();
    return result;
}

int MenuTestPgTransaction()
{
    TestSection("PG — Transaction (Begin / Commit / Rollback)");
    IoEnv env;
    int result = 0;

    auto conn = std::make_shared<gb::PgConnection>(env.io_ctx);
    gb::DbConfig cfg = DefaultPgConfig();

    // ── Connect ──
    bool ok = WaitValue<bool>([&](auto cb) {
        conn->AsyncConnect(cfg, [cb = std::move(cb)](bool ok) mutable { cb(ok); });
    });
    if (!ok) { TestResult("Connect", false); return -1; }
    TestResult("Connect", true);

    // ── Clean up old data at START ──
    {
        WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery("DROP TABLE IF EXISTS db_test_tx",
                [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
    }

    // ── CREATE TABLE ──
    {
        bool ct = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                CREATE TABLE db_test_tx (
                    id   SERIAL PRIMARY KEY,
                    name VARCHAR(50)
                )
            )SQL", [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        TestResult("CREATE TABLE db_test_tx", ct);
        if (!ct) { ++result; goto close; }
    }

    // ── Begin + Rollback ──
    {
        bool b1 = WaitValue<bool>([&](auto cb) {
            conn->AsyncBegin([cb = std::move(cb)](bool ok) mutable { cb(ok); });
        });
        TestResult("AsyncBegin (for rollback)", b1);
        if (!b1) { ++result; goto close; }

        bool ins = WaitValue<bool>([&](auto cb) {
            std::vector<gb::DbValue> params = {gb::DbValue("rollback_me")};
            conn->AsyncQuery(
                "INSERT INTO db_test_tx (name) VALUES ($1)",
                params,
                [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        TestResult("  INSERT 'rollback_me' in tx", ins);
        if (!ins) ++result;

        bool rb = WaitValue<bool>([&](auto cb) {
            conn->AsyncRollback([cb = std::move(cb)](bool ok) mutable { cb(ok); });
        });
        TestResult("AsyncRollback", rb);
        if (!rb) ++result;

        // Verify rollback: should be 0 rows
        int cnt = WaitValue<int>([&](auto cb) {
            conn->AsyncQuery("SELECT COUNT(*) FROM db_test_tx WHERE name = 'rollback_me'",
                [cb = std::move(cb)](gb::DbResult res) mutable {
                    int c = 0;
                    if (res.is_ok() && res.rows_count() > 0)
                        c = const_cast<gb::DbResult&>(res).next()[0].as_int32();
                    cb(c);
                });
        });
        TestResult("  Rollback verified (0 rows)", cnt == 0, "cnt=" + std::to_string(cnt));
        if (cnt != 0) ++result;
    }

    // ── Begin + Commit ──
    {
        bool b2 = WaitValue<bool>([&](auto cb) {
            conn->AsyncBegin([cb = std::move(cb)](bool ok) mutable { cb(ok); });
        });
        TestResult("AsyncBegin (for commit)", b2);
        if (!b2) { ++result; goto close; }

        bool ins = WaitValue<bool>([&](auto cb) {
            std::vector<gb::DbValue> params = {gb::DbValue("commit_me")};
            conn->AsyncQuery(
                "INSERT INTO db_test_tx (name) VALUES ($1)",
                params,
                [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        TestResult("  INSERT 'commit_me' in tx", ins);
        if (!ins) ++result;

        bool cm = WaitValue<bool>([&](auto cb) {
            conn->AsyncCommit([cb = std::move(cb)](bool ok) mutable { cb(ok); });
        });
        TestResult("AsyncCommit", cm);
        if (!cm) ++result;

        // Verify commit: should be 1 row
        int cnt = WaitValue<int>([&](auto cb) {
            conn->AsyncQuery("SELECT COUNT(*) FROM db_test_tx WHERE name = 'commit_me'",
                [cb = std::move(cb)](gb::DbResult res) mutable {
                    int c = 0;
                    if (res.is_ok() && res.rows_count() > 0)
                        c = const_cast<gb::DbResult&>(res).next()[0].as_int32();
                    cb(c);
                });
        });
        TestResult("  Commit verified (1 row)", cnt == 1, "cnt=" + std::to_string(cnt));
        if (cnt != 1) ++result;
    }

close:
    // NO DROP — data stays for inspection
    WaitDone([&](auto cb) {
        conn->AsyncClose([cb = std::move(cb)]() mutable { cb(); });
    });
    conn.reset();
    return result;
}

// ══════════════════════════════════════════════════════════════════════
// 新增: 子查询测试
// ══════════════════════════════════════════════════════════════════════

int MenuTestPgSubquery()
{
    TestSection("PG — Subquery (IN / EXISTS / Scalar / Correlated)");
    IoEnv env;
    int result = 0;

    auto conn = std::make_shared<gb::PgConnection>(env.io_ctx);
    gb::DbConfig cfg = DefaultPgConfig();

    // ── Connect ──
    bool ok = WaitValue<bool>([&](auto cb) {
        conn->AsyncConnect(cfg, [cb = std::move(cb)](bool ok) mutable { cb(ok); });
    });
    if (!ok) { TestResult("Connect", false); return -1; }
    TestResult("Connect", true);

    // ── Clean up old test data at START ──
    // Must drop tables with FK dependencies first (db_test_assign, db_test_project
    // may exist from a previous JOIN test run) before db_test_emp / db_test_dept.
    {
        WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery("DROP TABLE IF EXISTS db_test_assign",
                [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery("DROP TABLE IF EXISTS db_test_project",
                [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery("DROP TABLE IF EXISTS db_test_emp",
                [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery("DROP TABLE IF EXISTS db_test_dept",
                [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
    }

    // ── CREATE tables ──
    {
        bool ct1 = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                CREATE TABLE IF NOT EXISTS db_test_dept (
                    id   SERIAL PRIMARY KEY,
                    name VARCHAR(100) NOT NULL
                )
            )SQL", [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        TestResult("CREATE TABLE db_test_dept", ct1);
        if (!ct1) ++result;

        bool ct2 = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                CREATE TABLE IF NOT EXISTS db_test_emp (
                    id      SERIAL PRIMARY KEY,
                    name    VARCHAR(100) NOT NULL,
                    dept_id INTEGER REFERENCES db_test_dept(id),
                    salary  INTEGER DEFAULT 0
                )
            )SQL", [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        TestResult("CREATE TABLE db_test_emp", ct2);
        if (!ct2) ++result;
    }

    // ── Insert departments ──
    {
        bool ins = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                INSERT INTO db_test_dept (name) VALUES ('Engineering'), ('Sales'), ('HR'), ('R&D')
            )SQL",
                [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        TestResult("INSERT depts (Engineering,Sales,HR,R&D)", ins);
        if (!ins) ++result;
    }

    // ── Insert employees ──
    {
        bool ins = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                INSERT INTO db_test_emp (name, dept_id, salary) VALUES
                    ('Alice',   1, 8000),
                    ('Bob',     1, 9000),
                    ('Charlie', 2, 7000),
                    ('Diana',   2, 7500),
                    ('Eve',     3, 6500)
            )SQL",
                [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        TestResult("INSERT emps (Alice,Bob,Charlie,Diana,Eve)", ins);
        if (!ins) ++result;
    }

    std::cout << "  [DATA] db_test_dept: Engineering(id=1), Sales(id=2), HR(id=3), R&D(id=4)\n";
    std::cout << "  [DATA] db_test_emp: Alice(1,8000), Bob(1,9000), Charlie(2,7000), Diana(2,7500), Eve(3,6500)\n";

    // ── 1. Subquery with IN ──
    {
        int row_count = 0;
        std::vector<std::string> names;
        bool ok = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                SELECT e.name FROM db_test_emp e
                WHERE e.dept_id IN (
                    SELECT d.id FROM db_test_dept d WHERE d.name IN ('Engineering', 'Sales')
                )
                ORDER BY e.name
            )SQL",
                [&row_count, &names, cb = std::move(cb)](gb::DbResult res) mutable {
                    if (!res.is_ok()) { cb(false); return; }
                    row_count = (int)res.rows_count();
                    auto r = const_cast<gb::DbResult&>(res);
                    while (r.has_next())
                        names.push_back(std::string(r.next()[0].as_string()));
                    bool pass = (row_count == 4 &&
                                 names.size() == 4 &&
                                 names[0] == "Alice" && names[1] == "Bob" &&
                                 names[2] == "Charlie" && names[3] == "Diana");
                    cb(pass);
                });
        });
        std::string names_str;
        for (size_t i = 0; i < names.size(); ++i)
            names_str += (i > 0 ? "," : "") + names[i];
        std::string detail = "rows=" + std::to_string(row_count) + " names=[" + names_str + "]";
        TestResult("Subquery IN (Engineering,Sales) → 4 emps", ok, detail);
        if (!ok) ++result;
    }

    // ── 2. Subquery with EXISTS ──
    {
        int row_count = 0;
        bool ok = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                SELECT d.name FROM db_test_dept d
                WHERE EXISTS (
                    SELECT 1 FROM db_test_emp e WHERE e.dept_id = d.id
                )
                ORDER BY d.name
            )SQL",
                [&row_count, cb = std::move(cb)](gb::DbResult res) mutable {
                    if (!res.is_ok()) { cb(false); return; }
                    row_count = (int)res.rows_count();
                    auto r = const_cast<gb::DbResult&>(res);
                    std::vector<std::string> names;
                    while (r.has_next())
                        names.push_back(std::string(r.next()[0].as_string()));
                    // R&D has no employees, so EXISTS should exclude it
                    bool pass = (row_count == 3);
                    cb(pass);
                });
        });
        TestResult("EXISTS (exclude R&D with no emps)", ok,
                   "rows=" + std::to_string(row_count) + " (expect 3)");
        if (!ok) ++result;
    }

    // ── 3. Scalar subquery (MAX) ──
    {
        std::string emp_name;
        bool ok = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                SELECT e.name FROM db_test_emp e
                WHERE e.salary = (SELECT MAX(salary) FROM db_test_emp)
            )SQL",
                [&emp_name, cb = std::move(cb)](gb::DbResult res) mutable {
                    if (!res.is_ok() || res.rows_count() == 0) { cb(false); return; }
                    auto row = const_cast<gb::DbResult&>(res).next();
                    emp_name = std::string(row[0].as_string());
                    cb(emp_name == "Bob");
                });
        });
        TestResult("Scalar subquery MAX(salary) → Bob", ok,
                   "name=" + emp_name + " (expect Bob)");
        if (!ok) ++result;
    }

    // ── 4. Correlated subquery ──
    {
        int row_count = 0;
        bool ok = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                SELECT e.name FROM db_test_emp e
                WHERE e.salary > (
                    SELECT AVG(salary) FROM db_test_emp
                    WHERE dept_id = e.dept_id
                )
                ORDER BY e.name
            )SQL",
                [&row_count, cb = std::move(cb)](gb::DbResult res) mutable {
                    if (!res.is_ok()) { cb(false); return; }
                    row_count = (int)res.rows_count();
                    // Bob (9000 > avg 8500 Engineering), Diana (7500 > avg 7250 Sales)
                    // Eve (6500 NOT > 6500 HR avg)
                    cb(row_count == 2);
                });
        });
        std::string detail = "rows=" + std::to_string(row_count) + " (expect 2: Bob(9000>8500), Diana(7500>7250))";
        TestResult("Correlated subquery (salary > dept avg)", ok, detail);
        if (!ok) ++result;
    }

    // NO DROP — data stays for inspection
    WaitDone([&](auto cb) {
        conn->AsyncClose([cb = std::move(cb)]() mutable { cb(); });
    });
    conn.reset();
    return result;
}

// ══════════════════════════════════════════════════════════════════════
// 新增: JOIN 测试
// ══════════════════════════════════════════════════════════════════════

int MenuTestPgJoin()
{
    TestSection("PG — JOIN (INNER / LEFT / Multi-table / UNION)");
    IoEnv env;
    int result = 0;

    auto conn = std::make_shared<gb::PgConnection>(env.io_ctx);
    gb::DbConfig cfg = DefaultPgConfig();

    // ── Connect ──
    bool ok = WaitValue<bool>([&](auto cb) {
        conn->AsyncConnect(cfg, [cb = std::move(cb)](bool ok) mutable { cb(ok); });
    });
    if (!ok) { TestResult("Connect", false); return -1; }
    TestResult("Connect", true);

    // ── Clean up old test data at START ──
    {
        WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery("DROP TABLE IF EXISTS db_test_assign",
                [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery("DROP TABLE IF EXISTS db_test_project",
                [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery("DROP TABLE IF EXISTS db_test_emp",
                [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery("DROP TABLE IF EXISTS db_test_dept",
                [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
    }

    // ── CREATE tables ──
    {
        bool ct = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                CREATE TABLE db_test_dept (
                    id   SERIAL PRIMARY KEY,
                    name VARCHAR(100) NOT NULL
                )
            )SQL", [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        TestResult("CREATE TABLE db_test_dept", ct);
        if (!ct) ++result;

        ct = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                CREATE TABLE db_test_emp (
                    id      SERIAL PRIMARY KEY,
                    name    VARCHAR(100) NOT NULL,
                    dept_id INTEGER REFERENCES db_test_dept(id),
                    salary  INTEGER DEFAULT 0
                )
            )SQL", [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        TestResult("CREATE TABLE db_test_emp", ct);
        if (!ct) ++result;

        ct = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                CREATE TABLE db_test_project (
                    id   SERIAL PRIMARY KEY,
                    name VARCHAR(100) NOT NULL
                )
            )SQL", [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        TestResult("CREATE TABLE db_test_project", ct);
        if (!ct) ++result;

        ct = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                CREATE TABLE db_test_assign (
                    emp_id  INTEGER REFERENCES db_test_emp(id),
                    proj_id INTEGER REFERENCES db_test_project(id),
                    PRIMARY KEY (emp_id, proj_id)
                )
            )SQL", [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        TestResult("CREATE TABLE db_test_assign", ct);
        if (!ct) ++result;
    }

    // ── Insert data ──
    {
        bool ins = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                INSERT INTO db_test_dept (name) VALUES ('Engineering'), ('Sales'), ('HR'), ('R&D')
            )SQL", [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        TestResult("INSERT depts (Engineering,Sales,HR,R&D)", ins);
        if (!ins) ++result;

        ins = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                INSERT INTO db_test_emp (name, dept_id, salary) VALUES
                    ('Alice', 1, 8000),
                    ('Bob', 1, 9000),
                    ('Charlie', 2, 7000),
                    ('Diana', 2, 7500),
                    ('Eve', 3, 6500)
            )SQL", [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        TestResult("INSERT emps (5 emps)", ins);
        if (!ins) ++result;

        ins = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                INSERT INTO db_test_project (name) VALUES ('ProjectX'), ('ProjectY')
            )SQL", [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        TestResult("INSERT projects (ProjectX, ProjectY)", ins);
        if (!ins) ++result;

        ins = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                INSERT INTO db_test_assign (emp_id, proj_id) VALUES
                    (1, 1), (2, 1), (3, 2)
            )SQL", [cb = std::move(cb)](gb::DbResult res) mutable { cb(res.is_ok()); });
        });
        TestResult("INSERT assigns (Alice→X, Bob→X, Charlie→Y)", ins);
        if (!ins) ++result;
    }

    std::cout << "  [DATA] Depts: Engineering(1), Sales(2), HR(3), R&D(4)\n";
    std::cout << "  [DATA] Emps: Alice(Engineering,8000), Bob(Engineering,9000),\n";
    std::cout << "               Charlie(Sales,7000), Diana(Sales,7500), Eve(HR,6500)\n";
    std::cout << "  [DATA] ProjectX(1): Alice,Bob; ProjectY(2): Charlie\n";

    // ── 1. INNER JOIN ──
    {
        int row_count = 0;
        bool ok = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                SELECT e.name, d.name FROM db_test_emp e
                JOIN db_test_dept d ON e.dept_id = d.id
                ORDER BY e.name
            )SQL",
                [&row_count, cb = std::move(cb)](gb::DbResult res) mutable {
                    if (!res.is_ok()) { cb(false); return; }
                    row_count = (int)res.rows_count();
                    // All 5 emps have depts → 5 rows
                    cb(row_count == 5);
                });
        });
        TestResult("INNER JOIN emp+dept", ok,
                   "rows=" + std::to_string(row_count) + " (expect 5)");
        if (!ok) ++result;
    }

    // ── 2. INNER JOIN verify column values ──
    {
        bool ok = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                SELECT e.name, d.name, e.salary FROM db_test_emp e
                JOIN db_test_dept d ON e.dept_id = d.id
                WHERE e.name = 'Alice'
            )SQL",
                [cb = std::move(cb)](gb::DbResult res) mutable {
                    if (!res.is_ok() || res.rows_count() == 0) { cb(false); return; }
                    auto row = const_cast<gb::DbResult&>(res).next();
                    cb(std::string(row[0].as_string()) == "Alice" &&
                       std::string(row[1].as_string()) == "Engineering" &&
                       row[2].as_int32() == 8000);
                });
        });
        TestResult("  Alice → Engineering, salary=8000", ok);
        if (!ok) ++result;
    }

    // ── 3. LEFT JOIN (include R&D with no emps) ──
    {
        int row_count = 0;
        bool ok = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                SELECT d.name, e.name FROM db_test_dept d
                LEFT JOIN db_test_emp e ON d.id = e.dept_id
                ORDER BY d.name, e.name
            )SQL",
                [&row_count, cb = std::move(cb)](gb::DbResult res) mutable {
                    if (!res.is_ok()) { cb(false); return; }
                    row_count = (int)res.rows_count();
                    // Engineering(2) + Sales(2) + HR(1) + R&D(1 null) = 6 rows
                    cb(row_count == 6);
                });
        });
        TestResult("LEFT JOIN (R&D has NULL emp)", ok,
                   "rows=" + std::to_string(row_count) + " (expect 6)");
        if (!ok) ++result;
    }

    // ── 4. LEFT JOIN — verify R&D has NULL ──
    {
        bool ok = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                SELECT d.name, e.name FROM db_test_dept d
                LEFT JOIN db_test_emp e ON d.id = e.dept_id
                WHERE d.name = 'R&D'
            )SQL",
                [cb = std::move(cb)](gb::DbResult res) mutable {
                    if (!res.is_ok() || res.rows_count() == 0) { cb(false); return; }
                    auto r = const_cast<gb::DbResult&>(res);
                    auto row = r.next();
                    // e.name should be empty (NULL from LEFT JOIN)
                    cb(std::string(row[0].as_string()) == "R&D" &&
                       std::string(row[1].as_string()).empty());
                });
        });
        TestResult("  R&D → NULL employee (verified)", ok);
        if (!ok) ++result;
    }

    // ── 5. Multi-table JOIN (3 tables) ──
    {
        int row_count = 0;
        bool ok = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                SELECT e.name, p.name FROM db_test_emp e
                JOIN db_test_assign a ON e.id = a.emp_id
                JOIN db_test_project p ON a.proj_id = p.id
                ORDER BY e.name
            )SQL",
                [&row_count, cb = std::move(cb)](gb::DbResult res) mutable {
                    if (!res.is_ok()) { cb(false); return; }
                    row_count = (int)res.rows_count();
                    cb(row_count == 3);  // Alice→X, Bob→X, Charlie→Y
                });
        });
        TestResult("3-table JOIN emp+assign+project", ok,
                   "rows=" + std::to_string(row_count) + " (expect 3)");
        if (!ok) ++result;
    }

    // ── 6. Verify multi-table values ──
    {
        bool ok = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                SELECT e.name, p.name FROM db_test_emp e
                JOIN db_test_assign a ON e.id = a.emp_id
                JOIN db_test_project p ON a.proj_id = p.id
                WHERE e.name = 'Charlie'
            )SQL",
                [cb = std::move(cb)](gb::DbResult res) mutable {
                    if (!res.is_ok() || res.rows_count() == 0) { cb(false); return; }
                    auto row = const_cast<gb::DbResult&>(res).next();
                    cb(std::string(row[0].as_string()) == "Charlie" &&
                       std::string(row[1].as_string()) == "ProjectY");
                });
        });
        TestResult("  Charlie → ProjectY (verified)", ok);
        if (!ok) ++result;
    }

    // ── 7. UNION ALL ──
    {
        int row_count = 0;
        bool ok = WaitValue<bool>([&](auto cb) {
            conn->AsyncQuery(R"SQL(
                SELECT name, salary FROM db_test_emp WHERE dept_id = 1
                UNION ALL
                SELECT name, salary FROM db_test_emp WHERE dept_id = 2
            )SQL",
                [&row_count, cb = std::move(cb)](gb::DbResult res) mutable {
                    if (!res.is_ok()) { cb(false); return; }
                    row_count = (int)res.rows_count();
                    cb(row_count == 4);  // Alice,Bob (dept1) + Charlie,Diana (dept2)
                });
        });
        TestResult("UNION ALL dept1 + dept2", ok,
                   "rows=" + std::to_string(row_count) + " (expect 4)");
        if (!ok) ++result;
    }

    // NO DROP — data stays for inspection
    WaitDone([&](auto cb) {
        conn->AsyncClose([cb = std::move(cb)]() mutable { cb(); });
    });
    conn.reset();
    return result;
}

// ══════════════════════════════════════════════════════════════════════
// PG 协程 Query / Execute 测试
// ══════════════════════════════════════════════════════════════════════

int MenuTestPgCoroQuery()
{
    TestSection("PG — Coroutine Query/Execute (syncAwait)");
    IoEnv env;
    int result = 0;

    auto conn = std::make_shared<gb::PgConnection>(env.io_ctx);
    gb::DbConfig cfg = DefaultPgConfig();

    // ── Connect via coroutine ──
    bool ok = async_simple::coro::syncAwait(conn->Connect(cfg));
    if (!ok) { TestResult("Connect", false); return -1; }
    TestResult("Connect (coro)", true);

    TestResult("IsConnected after connect", conn->IsConnected());

    // ── Clean up old test data at START ──
    async_simple::coro::syncAwait(conn->Query("DROP TABLE IF EXISTS db_test_coro"));

    // ── CREATE TABLE via coroutine ──
    {
        auto res = async_simple::coro::syncAwait(conn->Query(R"SQL(
            CREATE TABLE IF NOT EXISTS db_test_coro (
                id   SERIAL PRIMARY KEY,
                val  INTEGER
            )
        )SQL"));
        TestResult("CREATE TABLE db_test_coro", res.is_ok());
        if (!res.is_ok()) ++result;
    }

    // ── INSERT via coroutine ──
    {
        auto res = async_simple::coro::syncAwait(
            conn->Query("INSERT INTO db_test_coro (val) VALUES ($1),($2),($3)",
                        std::vector<gb::DbValue>{gb::DbValue(10), gb::DbValue(20), gb::DbValue(30)}));
        TestResult("INSERT 3 rows (10,20,30)", res.is_ok());
        if (!res.is_ok()) ++result;
    }

    // ── SELECT via coroutine — verify count ──
    {
        auto res = async_simple::coro::syncAwait(conn->Query("SELECT COUNT(*) FROM db_test_coro"));
        bool pass = res.is_ok() && res.rows_count() > 0 &&
                    const_cast<gb::DbResult&>(res).next()[0].as_int32() == 3;
        TestResult("COUNT after INSERT", pass,
                   pass ? "cnt=3" : "failed");
        if (!pass) ++result;
    }

    // ── SELECT values via coroutine ──
    {
        auto res = async_simple::coro::syncAwait(
            conn->Query("SELECT val FROM db_test_coro ORDER BY val"));
        bool pass = res.is_ok() && res.rows_count() == 3;
        if (pass) {
            auto r = const_cast<gb::DbResult&>(res);
            pass = r.next()[0].as_int32() == 10 &&
                   r.next()[0].as_int32() == 20 &&
                   r.next()[0].as_int32() == 30;
        }
        TestResult("SELECT verify values (10,20,30)", pass,
                   pass ? "10,20,30" : "failed");
        if (!pass) ++result;
    }

    // ── Execute via coroutine — UPDATE ──
    {
        uint64_t n = async_simple::coro::syncAwait(
            conn->Execute("UPDATE db_test_coro SET val = val + 100 WHERE val = 10"));
        TestResult("Execute UPDATE val+100 WHERE val=10", n > 0,
                   "n=" + std::to_string(n));
        if (n == 0) ++result;
    }

    // ── Parameterized query — verify updated value ──
    {
        auto res = async_simple::coro::syncAwait(
            conn->Query("SELECT val FROM db_test_coro WHERE val = $1",
                        std::vector<gb::DbValue>{gb::DbValue(110)}));
        bool pass = res.is_ok() && res.rows_count() == 1 &&
                    const_cast<gb::DbResult&>(res).next()[0].as_int32() == 110;
        TestResult("Verify val=110 after UPDATE", pass,
                   pass ? "found" : "not found");
        if (!pass) ++result;
    }

    // ── Close via coroutine ──
    async_simple::coro::syncAwait(conn->Close());
    TestResult("Close (IsConnected=false)", !conn->IsConnected());

    conn.reset();
    return result;
}

// ══════════════════════════════════════════════════════════════════════
// PG 协程事务 (Begin / Commit / Rollback) 测试
// ══════════════════════════════════════════════════════════════════════

int MenuTestPgCoroTransaction()
{
    TestSection("PG — Coroutine Transaction (Begin/Commit/Rollback)");
    IoEnv env;
    int result = 0;

    auto conn = std::make_shared<gb::PgConnection>(env.io_ctx);
    gb::DbConfig cfg = DefaultPgConfig();

    // ── Connect via coroutine ──
    bool ok = async_simple::coro::syncAwait(conn->Connect(cfg));
    if (!ok) { TestResult("Connect", false); return -1; }
    TestResult("Connect (coro)", true);

    // ── Clean up old test data at START ──
    async_simple::coro::syncAwait(conn->Query("DROP TABLE IF EXISTS db_test_coro_tx"));

    // ── CREATE TABLE via coroutine ──
    {
        auto res = async_simple::coro::syncAwait(conn->Query(R"SQL(
            CREATE TABLE db_test_coro_tx (
                id   SERIAL PRIMARY KEY,
                name VARCHAR(50)
            )
        )SQL"));
        TestResult("CREATE TABLE db_test_coro_tx", res.is_ok());
        if (!res.is_ok()) ++result;
    }

    // ── Begin + Rollback ──
    {
        async_simple::coro::syncAwait(conn->Begin());
        TestResult("Begin (for rollback)", true);

        auto res = async_simple::coro::syncAwait(
            conn->Query("INSERT INTO db_test_coro_tx (name) VALUES ($1)",
                        std::vector<gb::DbValue>{gb::DbValue("rollback_me")}));
        TestResult("  INSERT 'rollback_me' in tx", res.is_ok());
        if (!res.is_ok()) ++result;

        async_simple::coro::syncAwait(conn->Rollback());
        TestResult("Rollback", true);

        // Verify rollback: should be 0 rows
        auto check = async_simple::coro::syncAwait(
            conn->Query("SELECT COUNT(*) FROM db_test_coro_tx WHERE name = 'rollback_me'"));
        int cnt = (check.is_ok() && check.rows_count() > 0)
                    ? const_cast<gb::DbResult&>(check).next()[0].as_int32() : -1;
        TestResult("  Rollback verified (0 rows)", cnt == 0, "cnt=" + std::to_string(cnt));
        if (cnt != 0) ++result;
    }

    // ── Begin + Commit ──
    {
        async_simple::coro::syncAwait(conn->Begin());
        TestResult("Begin (for commit)", true);

        auto res = async_simple::coro::syncAwait(
            conn->Query("INSERT INTO db_test_coro_tx (name) VALUES ($1)",
                        std::vector<gb::DbValue>{gb::DbValue("commit_me")}));
        TestResult("  INSERT 'commit_me' in tx", res.is_ok());
        if (!res.is_ok()) ++result;

        async_simple::coro::syncAwait(conn->Commit());
        TestResult("Commit", true);

        // Verify commit: should be 1 row
        auto check = async_simple::coro::syncAwait(
            conn->Query("SELECT COUNT(*) FROM db_test_coro_tx WHERE name = 'commit_me'"));
        int cnt = (check.is_ok() && check.rows_count() > 0)
                    ? const_cast<gb::DbResult&>(check).next()[0].as_int32() : -1;
        TestResult("  Commit verified (1 row)", cnt == 1, "cnt=" + std::to_string(cnt));
        if (cnt != 1) ++result;
    }

    // ── Close via coroutine ──
    async_simple::coro::syncAwait(conn->Close());
    conn.reset();
    return result;
}
