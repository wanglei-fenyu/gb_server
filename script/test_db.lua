-- ═══════════════════════════════════════════════════════════════════
-- Redis + PostgreSQL Lua 综合测试
--
-- 运行方式：在 main.lua 末尾添加 require("test_db")
-- 所有测试是异步的，结果通过回调打印到日志。
-- ═══════════════════════════════════════════════════════════════════

local test = {}
local total = 0
local passed = 0
local failed = 0

-- ── 测试辅助 ──

local function log_test(name, ok, detail)
    total = total + 1
    if ok then
        passed = passed + 1
        log.Info(string.format("[PASS] %s", name))
    else
        failed = failed + 1
        log.Error(string.format("[FAIL] %s -- %s", name, detail or ""))
    end
end

local function log_section(title)
    log.Info(string.rep("=", 60))
    log.Info("  " .. title)
    log.Info(string.rep("=", 60))
end

-- ═════════════════════════════════════════════════════════════════════════
-- Redis Lua Tests
-- ═════════════════════════════════════════════════════════════════════════

local function run_redis_tests()
    log_section("Redis Lua Async Callback Tests")

    -- ── AsyncSet / AsyncGet / AsyncDel ──
    redis.AsyncSet("lua:str", "hello_lua", function(err)
        log_test("AsyncSet", err == "", err)
        if err == "" then
            redis.AsyncGet("lua:str", function(err2, val)
                log_test("AsyncGet", err2 == "" and val == "hello_lua",
                         string.format("err=%s val=%s", err2, tostring(val)))
                redis.AsyncDel("lua:str", function(err3, n)
                    log_test("AsyncDel", err3 == "" and n > 0,
                             string.format("err=%s n=%d", err3, n or 0))
                end)
            end)
        end
    end)

    -- ── AsyncSetEx / AsyncExists ──
    redis.AsyncSetEx("lua:ex", "ttl_val", 100, function(err)
        log_test("AsyncSetEx", err == "", err)
        redis.AsyncExists("lua:ex", function(err2, exists)
            log_test("AsyncExists (after SetEx)", err2 == "" and exists,
                     string.format("err=%s exists=%s", err2, tostring(exists)))
        end)
    end)

    -- ── AsyncIncr / AsyncIncrBy ──
    redis.AsyncIncr("lua:incr", function(err, n1)
        log_test("AsyncIncr #1", err == "" and n1 == 1,
                 string.format("err=%s val=%d", err, n1 or -1))
        redis.AsyncIncr("lua:incr", function(err2, n2)
            log_test("AsyncIncr #2", err2 == "" and n2 == 2,
                     string.format("err=%s val=%d", err2, n2 or -1))
            redis.AsyncIncrBy("lua:incr", 10, function(err3, n3)
                log_test("AsyncIncrBy +10", err3 == "" and n3 == 12,
                         string.format("err=%s val=%d", err3, n3 or -1))
                redis.AsyncDel("lua:incr", function() end)
            end)
        end)
    end)

    -- ── AsyncHSet / AsyncHGet / AsyncHDel / AsyncHKeys / AsyncHVals / AsyncHLen ──
    redis.AsyncHSet("lua:hash", "field1", "val1", function(err, n)
        log_test("AsyncHSet (new)", err == "" and n == 1,
                 string.format("err=%s n=%d", err, n or -1))
        redis.AsyncHSet("lua:hash", "field1", "val1_upd", function(err2, n2)
            log_test("AsyncHSet (update)", err2 == "" and n2 == 0,
                     string.format("err=%s n=%d", err2, n2 or -1))
            redis.AsyncHGet("lua:hash", "field1", function(err3, v)
                log_test("AsyncHGet", err3 == "" and v == "val1_upd",
                         string.format("err=%s val=%s", err3, tostring(v)))
                redis.AsyncHLen("lua:hash", function(err4, len)
                    log_test("AsyncHLen", err4 == "" and len >= 1,
                             string.format("err=%s len=%d", err4, len or 0))
                    redis.AsyncHKeys("lua:hash", function(err5, keys)
                        log_test("AsyncHKeys", err5 == "" and #keys >= 1,
                                 string.format("err=%s keys=%d", err5, #(keys or {})))
                        redis.AsyncHVals("lua:hash", function(err6, vals)
                            log_test("AsyncHVals", err6 == "" and #vals >= 1,
                                     string.format("err=%s vals=%d", err6, #(vals or {})))
                            redis.AsyncHDel("lua:hash", "field1", function(err7, dn)
                                log_test("AsyncHDel", err7 == "" and dn > 0,
                                         string.format("err=%s n=%d", err7, dn or 0))
                            end)
                        end)
                    end)
                end)
            end)
        end)
    end)

    -- ── AsyncLPush / AsyncRPush / AsyncLPop / AsyncRPop / AsyncLLen ──
    redis.AsyncLPush("lua:list", "item1", function(err, n)
        log_test("AsyncLPush #1", err == "" and n >= 1,
                 string.format("err=%s len=%d", err, n or -1))
        redis.AsyncRPush("lua:list", "item2", function(err2, n2)
            log_test("AsyncRPush #2", err2 == "" and n2 >= 2,
                     string.format("err=%s len=%d", err2, n2 or -1))
            redis.AsyncLLen("lua:list", function(err3, len)
                log_test("AsyncLLen", err3 == "" and len >= 2,
                         string.format("err=%s len=%d", err3, len or 0))
                redis.AsyncLPop("lua:list", function(err4, v1)
                    log_test("AsyncLPop", err4 == "" and v1 ~= nil and v1 ~= "",
                             string.format("err=%s val=%s", err4, tostring(v1)))
                    redis.AsyncRPop("lua:list", function(err5, v2)
                        log_test("AsyncRPop", err5 == "" and v2 ~= nil and v2 ~= "",
                                 string.format("err=%s val=%s", err5, tostring(v2)))
                    end)
                end)
            end)
        end)
    end)

    -- ── AsyncZAdd / AsyncZCard / AsyncZScore / AsyncZRank / AsyncZRem ──
    redis.AsyncZAdd("lua:zset", 10.5, "member_a", function(err, n)
        log_test("AsyncZAdd #1", err == "" and n == 1,
                 string.format("err=%s n=%d", err, n or -1))
        redis.AsyncZAdd("lua:zset", 20.0, "member_b", function(err2, n2)
            log_test("AsyncZAdd #2", err2 == "" and n2 == 1,
                     string.format("err=%s n=%d", err2, n2 or -1))
            redis.AsyncZAdd("lua:zset", 5.0, "member_c", function(err3, n3)
                log_test("AsyncZAdd #3 (lowest)", err3 == "" and n3 == 1,
                         string.format("err=%s n=%d", err3, n3 or -1))
                redis.AsyncZCard("lua:zset", function(err4, cnt)
                    log_test("AsyncZCard", err4 == "" and cnt == 3,
                             string.format("err=%s cnt=%d", err4, cnt or 0))
                    redis.AsyncZScore("lua:zset", "member_b", function(err5, score)
                        log_test("AsyncZScore (member_b)", err5 == "" and math.abs(score - 20.0) < 0.001,
                                 string.format("err=%s score=%.1f", err5, score or -1))
                        redis.AsyncZRank("lua:zset", "member_c", function(err6, rank)
                            log_test("AsyncZRank (member_c=0)", err6 == "" and rank == 0,
                                     string.format("err=%s rank=%d", err6, rank or -1))
                            redis.AsyncZRem("lua:zset", "member_a", function(err7, rn)
                                log_test("AsyncZRem", err7 == "" and rn == 1,
                                         string.format("err=%s n=%d", err7, rn or 0))
                            end)
                        end)
                    end)
                end)
            end)
        end)
    end)

    -- ── AsyncExpire / AsyncTTL ──
    redis.AsyncSet("lua:expire", "tmp", function(err)
        if err == "" then
            redis.AsyncExpire("lua:expire", 3600, function(err2)
                log_test("AsyncExpire", err2 == "", err2)
                redis.AsyncTTL("lua:expire", function(err3, ttl)
                    log_test("AsyncTTL", err3 == "" and ttl and ttl > 0 and ttl <= 3600,
                             string.format("err=%s ttl=%d", err3, ttl or -1))
                end)
            end)
        end
    end)

    -- ── AsyncPing ──
    redis.AsyncPing(function(err, ok)
        log_test("AsyncPing", err == "" and ok,
                 string.format("err=%s ok=%s", err, tostring(ok)))
    end)

    -- ── AsyncCall (generic GET/SET/EXISTS) ──
    redis.AsyncCall("SET", "lua:call_k", "call_v", function(err, result)
        log_test("AsyncCall SET", err == "", err)
        redis.AsyncCall("GET", "lua:call_k", function(err2, val)
            log_test("AsyncCall GET", err2 == "" and val == "call_v",
                     string.format("err=%s val=%s", err2, tostring(val)))
            redis.AsyncCall("EXISTS", "lua:call_k", function(err3, exists)
                log_test("AsyncCall EXISTS", err3 == "" and exists == 1,
                         string.format("err=%s exists=%s", err3, tostring(exists)))
                redis.AsyncCall("DEL", "lua:call_k", function(err4, dn)
                    log_test("AsyncCall DEL", err4 == "" and dn == 1,
                             string.format("err=%s n=%d", err4, dn or 0))
                end)
            end)
        end)
    end)

    -- ── AsyncEval ──
    redis.AsyncEval("return KEYS[1]", {"lua:eval_k"}, {}, function(err, val)
        log_test("AsyncEval return KEYS[1]", err == "" and val == "lua:eval_k",
                 string.format("err=%s val=%s", err, tostring(val)))
    end)

    redis.AsyncEval("return {KEYS[1], ARGV[1]}", {"k1", "k2"}, {"a1", "a2"},
        function(err, arr)
            local ok = err == "" and type(arr) == "table" and arr[1] == "k1" and arr[2] == "a1"
            log_test("AsyncEval multi key/arg", ok,
                     string.format("err=%s arr[1]=%s arr[2]=%s", err,
                                   tostring(arr and arr[1]), tostring(arr and arr[2])))
        end)

    -- ═════════════════════════════════════════════════════════════════════
    -- Redis Lua Coroutine (Await) Tests
    -- ═════════════════════════════════════════════════════════════════════

    log_section("Redis Lua Await (Coroutine) Tests")

    local co = coroutine.create(function()
        -- Await Set
        local e1 = redis.Await("Set", "lua:aw_str", "await_val")
        log_test("Await Set", e1 == "", e1)

        -- Await Get
        local e2, v2 = redis.Await("Get", "lua:aw_str")
        log_test("Await Get", e2 == "" and v2 == "await_val",
                 string.format("err=%s val=%s", e2, tostring(v2)))

        -- Await Del
        local e3, n3 = redis.Await("Del", "lua:aw_str")
        log_test("Await Del", e3 == "" and n3 and n3 > 0,
                 string.format("err=%s n=%d", e3, n3 or 0))

        -- Await HSet / HGet
        local e4, _ = redis.Await("HSet", "lua:aw_hash", "f1", "hv1")
        log_test("Await HSet", e4 == "", e4)
        local e5, v5 = redis.Await("HGet", "lua:aw_hash", "f1")
        log_test("Await HGet", e5 == "" and v5 == "hv1",
                 string.format("err=%s val=%s", e5, tostring(v5)))
        redis.AsyncDel("lua:aw_hash", function() end)

        -- Await LPush / LPop
        local e6, _ = redis.Await("LPush", "lua:aw_list", "li1")
        log_test("Await LPush", e6 == "", e6)
        local e7, v7 = redis.Await("LPop", "lua:aw_list")
        log_test("Await LPop", e7 == "" and v7 == "li1",
                 string.format("err=%s val=%s", e7, tostring(v7)))

        -- Await ZAdd / ZScore / ZCard
        local e8, _ = redis.Await("ZAdd", "lua:aw_zset", 88.8, "zmember")
        log_test("Await ZAdd", e8 == "", e8)
        local e9, s9 = redis.Await("ZScore", "lua:aw_zset", "zmember")
        log_test("Await ZScore", e9 == "" and math.abs(s9 - 88.8) < 0.001,
                 string.format("err=%s score=%.1f", e9, s9 or -1))
        local e10, c10 = redis.Await("ZCard", "lua:aw_zset")
        log_test("Await ZCard", e10 == "" and c10 == 1,
                 string.format("err=%s cnt=%d", e10, c10 or 0))
        redis.AsyncDel("lua:aw_zset", function() end)

        -- Await Expire / TTL
        local e11 = redis.Await("Set", "lua:aw_exp", "ex")
        if e11 == "" then
            local e12 = redis.Await("Expire", "lua:aw_exp", 7200)
            log_test("Await Expire", e12 == "", e12)
            local e13, ttl = redis.Await("TTL", "lua:aw_exp")
            log_test("Await TTL", e13 == "" and ttl and ttl > 0,
                     string.format("err=%s ttl=%d", e13, ttl or -1))
            redis.AsyncDel("lua:aw_exp", function() end)
        end

        -- Await Ping
        local e14, pong = redis.Await("Ping")
        log_test("Await Ping", e14 == "" and pong,
                 string.format("err=%s ok=%s", e14, tostring(pong)))

        -- Await Incr
        local e15, n15 = redis.Await("Incr", "lua:aw_incr")
        log_test("Await Incr", e15 == "" and n15 == 1,
                 string.format("err=%s val=%d", e15, n15 or -1))
        redis.AsyncDel("lua:aw_incr", function() end)

        log.Info("[Redis Lua tests completed]")
    end)
    coroutine.resume(co)
end

-- ═════════════════════════════════════════════════════════════════════════
-- PostgreSQL Lua Tests
-- ═════════════════════════════════════════════════════════════════════════

local function run_pg_tests()
    log_section("PostgreSQL Lua Async Callback Tests")

    local pg_cfg = {
        host     = "192.168.31.186",
        port     = 5432,
        user     = "fys",
        password = "fengyu",
        database = "mydb",
    }

    -- ── AsyncConnect ──
    pg.AsyncConnect(pg_cfg, function(err, ok)
        log_test("PG AsyncConnect", err == "" and ok,
                 string.format("err=%s ok=%s", err, tostring(ok)))
        if not ok then
            log.Error("  PG tests skipped — cannot connect")
            return
        end

        -- ── AsyncQuery (simple) ──
        pg.AsyncQuery("SELECT 42 AS answer, 'pg_lua' AS txt", function(err2, rows)
            log_test("PG AsyncQuery SELECT literal",
                     err2 == "" and rows and rows[1] and rows[1].answer == 42,
                     string.format("err=%s answer=%s", err2,
                                   tostring(rows and rows[1] and rows[1].answer)))
        end)

        -- ── AsyncQuery (parameterized) + AsyncExecute ──
        pg.AsyncExecute("DROP TABLE IF EXISTS gb_lua_test", function(err_drop, n)
            -- Create table
            pg.AsyncQuery("CREATE TABLE gb_lua_test (id SERIAL PRIMARY KEY, name TEXT, score INTEGER)",
                function(err_ct, r)
                    log_test("PG CREATE TABLE", err_ct == "", err_ct)
                    if err_ct ~= "" then return end

                    -- Parameterized INSERT
                    pg.AsyncQuery(
                        "INSERT INTO gb_lua_test (name, score) VALUES ($1, $2) RETURNING id",
                        "alice", 95,
                        function(err_ins, rows_ins)
                            log_test("PG AsyncQuery INSERT (param)",
                                     err_ins == "" and rows_ins and rows_ins[1] and rows_ins[1].id > 0,
                                     string.format("err=%s id=%s", err_ins,
                                                   tostring(rows_ins and rows_ins[1] and rows_ins[1].id)))

                            -- Parameterized SELECT
                            pg.AsyncQuery("SELECT * FROM gb_lua_test WHERE name = $1", "alice",
                                function(err_sel, rows_sel)
                                    local found = rows_sel and rows_sel[1] and rows_sel[1].name == "alice"
                                    log_test("PG AsyncQuery SELECT (param)",
                                             err_sel == "" and found,
                                             string.format("err=%s name=%s", err_sel,
                                                           tostring(rows_sel and rows_sel[1] and rows_sel[1].name)))

                                    -- AsyncExecute UPDATE
                                    pg.AsyncExecute(
                                        "UPDATE gb_lua_test SET score = 100 WHERE name = 'alice'",
                                        function(err_upd, n_upd)
                                            log_test("PG AsyncExecute UPDATE",
                                                     err_upd == "" and n_upd and n_upd > 0,
                                                     string.format("err=%s n=%d", err_upd, n_upd or 0))
                                        end)

                                    -- AsyncExecute DELETE
                                    pg.AsyncExecute("DELETE FROM gb_lua_test WHERE name = 'alice'",
                                        function(err_del, n_del)
                                            log_test("PG AsyncExecute DELETE",
                                                     err_del == "" and n_del and n_del > 0,
                                                     string.format("err=%s n=%d", err_del, n_del or 0))
                                        end)
                                end)
                        end)
                end)
        end)

        -- ── AsyncBegin / AsyncCommit ──
        pg.AsyncBegin(function(err_b, ok_b)
            log_test("PG AsyncBegin", err_b == "" and ok_b,
                     string.format("err=%s ok=%s", err_b, tostring(ok_b)))
            if not ok_b then return end

            pg.AsyncQuery(
                "INSERT INTO gb_lua_test (name, score) VALUES ('bob', 80)",
                function(err_i, _)
                    log_test("PG Insert (in tx, will commit)", err_i == "", err_i)

                    pg.AsyncCommit(function(err_c, ok_c)
                        log_test("PG AsyncCommit", err_c == "" and ok_c,
                                 string.format("err=%s ok=%s", err_c, tostring(ok_c)))
                    end)
                end)
        end)

        -- ── AsyncBegin / AsyncRollback ──
        pg.AsyncBegin(function(err_b2, ok_b2)
            log_test("PG AsyncBegin (for rollback)", err_b2 == "" and ok_b2,
                     string.format("err=%s ok=%s", err_b2, tostring(ok_b2)))
            if not ok_b2 then return end

            pg.AsyncQuery(
                "INSERT INTO gb_lua_test (name, score) VALUES ('carol', 70)",
                function(err_i2, _)
                    log_test("PG Insert (in tx, will rollback)", err_i2 == "", err_i2)

                    pg.AsyncRollback(function(err_r, ok_r)
                        log_test("PG AsyncRollback", err_r == "" and ok_r,
                                 string.format("err=%s ok=%s", err_r, tostring(ok_r)))
                    end)
                end)
        end)

        -- ── AsyncClose ──
        pg.AsyncClose(function(err_cl)
            log_test("PG AsyncClose", err_cl == "", err_cl)
        end)

        -- ═════════════════════════════════════════════════════════════════
        -- PostgreSQL Lua Await (Coroutine) Tests
        -- ═════════════════════════════════════════════════════════════════

        log_section("PostgreSQL Lua Await (Coroutine) Tests")

        local co = coroutine.create(function()
            -- Reconnect via Await
            local e1, ok1 = pg.Await("Connect", pg_cfg)
            log_test("PG Await Connect", e1 == "" and ok1,
                     string.format("err=%s ok=%s", e1, tostring(ok1)))
            if not ok1 then return end

            -- Await Query
            local e2, rows2 = pg.Await("Query", "SELECT 1 AS one, 'two' AS two")
            log_test("PG Await Query",
                     e2 == "" and rows2 and rows2[1] and rows2[1].one == 1 and rows2[1].two == "two",
                     string.format("err=%s one=%s two=%s", e2,
                                   tostring(rows2 and rows2[1] and rows2[1].one),
                                   tostring(rows2 and rows2[1] and rows2[1].two)))

            -- Await Query with params
            local e3, rows3 = pg.Await("Query",
                "SELECT $1::int + $2::int AS sum", 40, 2)
            log_test("PG Await Query (param)",
                     e3 == "" and rows3 and rows3[1] and rows3[1].sum == 42,
                     string.format("err=%s sum=%s", e3,
                                   tostring(rows3 and rows3[1] and rows3[1].sum)))

            -- Await Execute
            local e4, n4 = pg.Await("Execute", "DELETE FROM gb_lua_test WHERE name = 'bob'")
            log_test("PG Await Execute", e4 == "", e4)

            -- Await Begin + Commit
            local e5, ok5 = pg.Await("Begin")
            log_test("PG Await Begin", e5 == "" and ok5, e5)
            if ok5 then
                local e6, _ = pg.Await("Query",
                    "INSERT INTO gb_lua_test (name, score) VALUES ('dave', 60)")
                log_test("PG Await Insert (in tx)", e6 == "", e6)
                local e7, ok7 = pg.Await("Commit")
                log_test("PG Await Commit", e7 == "" and ok7, e7)
            end

            -- Await Begin + Rollback
            local e8, ok8 = pg.Await("Begin")
            log_test("PG Await Begin (for rollback)", e8 == "" and ok8, e8)
            if ok8 then
                local e9 = pg.Await("Execute", "DELETE FROM gb_lua_test WHERE name = 'dave'")
                log_test("PG Await Execute (in tx, will rollback)", e9 == "", e9)
                local e10, ok10 = pg.Await("Rollback")
                log_test("PG Await Rollback", e10 == "" and ok10, e10)
            end

            -- IsConnected
            local connected = pg.IsConnected()
            log_test("PG IsConnected", connected == true,
                     string.format("connected=%s", tostring(connected)))

            -- Cleanup table
            local e11, _ = pg.Await("Query", "DROP TABLE IF EXISTS gb_lua_test")
            log_test("PG Await DROP TABLE", e11 == "", e11)

            -- Close
            local e12 = pg.Await("Close")
            log_test("PG Await Close", e12 == "", e12)

            log.Info("[PG Lua tests completed]")
        end)
        coroutine.resume(co)
    end)
end

-- ═════════════════════════════════════════════════════════════════════════
-- Entry point
-- ═════════════════════════════════════════════════════════════════════════

log.Info("=== Starting DB Lua integration tests ===")

-- Run Redis tests first (Redis is usually available)
run_redis_tests()

-- Then PG tests
run_pg_tests()

-- Print summary after a short delay (all tests are async, but
-- results are printed as callbacks fire)
-- We schedule a delayed check via a timer if available, but for now
-- just note that results appear asynchronously.
log.Info("=== DB Lua tests scheduled (results appear above as callbacks fire) ===")
