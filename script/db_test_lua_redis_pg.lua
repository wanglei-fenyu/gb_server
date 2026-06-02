-- =====================================================================
-- db_test_lua_redis_pg.lua
--
-- Lua 侧 Redis / PostgreSQL 绑定测试。
-- 被 test/db_test/lua_test.cpp 以绝对路径加载。
--
-- 约定：
--   每个 test_* 函数使用全局的 redis_state / pg_state 表来汇报结果：
--     .done = false  → C++ 驱动将轮询 ProcessFrame 直到 .done == true
--     .ok   = bool   → 测试是否通过
--     .msg  = string → 结果描述
-- =====================================================================

-- ═══════════════════════════════════════════════════════════════════
-- Redis 测试
-- ═══════════════════════════════════════════════════════════════════

redis_state = { done = false, ok = false, msg = "" }

-- 初始化 Redis 连接池（必须在任意 Async* 之前调用）
redis.Connect({
    host     = "192.168.31.186",
    port     = 6379,
    password = "fengyu",
    db       = 0,
    pool_size = 2,
    timeout  = 5000
})

local function reset_redis()
    redis_state.done = false
    redis_state.ok   = false
    redis_state.msg  = ""
end

-- ── 1. AsyncPing ────────────────────────────────────────────────────
function test_redis_ping()
    reset_redis()
    redis.AsyncPing(function(err, ok)
        redis_state.done = true
        if err ~= "" then
            redis_state.msg = "AsyncPing error: " .. err
        elseif not ok then
            redis_state.msg = "AsyncPing returned false"
        else
            redis_state.ok  = true
            redis_state.msg = "ok"
        end
    end)
end

-- ── 2. AsyncSet + AsyncGet ─────────────────────────────────────────
function test_redis_set_get()
    reset_redis()
    local key = "db_test_lua_kv"
    local val = "hello_redis_" .. math.random(10000, 99999)

    redis.AsyncSet(key, val, function(err)
        if err ~= "" then
            redis_state.msg  = "AsyncSet error: " .. err
            redis_state.done = true
            return
        end
        redis.AsyncGet(key, function(err2, got)
            if err2 ~= "" then
                redis_state.msg = "AsyncGet error: " .. err2
            elseif got ~= val then
                redis_state.msg = "value mismatch: got '" .. tostring(got) .. "'"
            else
                redis_state.ok  = true
                redis_state.msg = "ok"
            end
            redis_state.done = true
        end)
    end)
end

-- ── 3. AsyncHSet + AsyncHGet + AsyncHDel ───────────────────────────
function test_redis_hash()
    reset_redis()
    local key   = "db_test_lua_hash"
    local field = "title"
    local val   = "hello_hash"

    -- 先删除确保干净
    redis.AsyncHDel(key, field, function()
        redis.AsyncHSet(key, field, val, function(err)
            if err ~= "" then
                redis_state.msg  = "AsyncHSet error: " .. err
                redis_state.done = true
                return
            end
            redis.AsyncHGet(key, field, function(err2, hval)
                if err2 ~= "" then
                    redis_state.msg = "AsyncHGet error: " .. err2
                elseif hval ~= val then
                    redis_state.msg = "HGet mismatch: " .. tostring(hval)
                else
                    redis_state.ok  = true
                    redis_state.msg = "ok"
                end
                redis_state.done = true
            end)
        end)
    end)
end

-- ── 4. AsyncCall (泛型命令) ──────────────────────────────────────
function test_redis_async_call()
    reset_redis()
    redis.AsyncCall("PING", function(err, result)
        redis_state.done = true
        if err ~= "" then
            redis_state.msg = "AsyncCall PING error: " .. err
        else
            redis_state.ok  = true
            redis_state.msg = "ok"
        end
    end)
end

-- ── 5. AsyncIncr ────────────────────────────────────────────────────
function test_redis_incr()
    reset_redis()
    local key = "db_test_lua_incr"
    redis.AsyncDel(key, function()
        redis.AsyncIncr(key, function(err, n)
            if err ~= "" then
                redis_state.msg  = "AsyncIncr error: " .. err
                redis_state.done = true
                return
            end
            if n ~= 1 then
                redis_state.msg = "Incr expected 1, got " .. tostring(n)
                redis_state.done = true
                return
            end
            redis.AsyncIncrBy(key, 5, function(err2, n2)
                if err2 ~= "" then
                    redis_state.msg = "AsyncIncrBy error: " .. err2
                elseif n2 ~= 6 then
                    redis_state.msg = "IncrBy expected 6, got " .. tostring(n2)
                else
                    redis_state.ok  = true
                    redis_state.msg = "ok"
                end
                redis_state.done = true
            end)
        end)
    end)
end

-- ── 6. AsyncExpire / AsyncTTL ──────────────────────────────────────
function test_redis_expire_ttl()
    reset_redis()
    local key = "db_test_lua_ttl"
    redis.AsyncSet(key, "tmp", function(err)
        if err ~= "" then
            redis_state.msg  = "Set error: " .. err
            redis_state.done = true
            return
        end
        redis.AsyncExpire(key, 100, function(err2)
            if err2 ~= "" then
                redis_state.msg  = "Expire error: " .. err2
                redis_state.done = true
                return
            end
            redis.AsyncTTL(key, function(err3, ttl)
                redis_state.done = true
                if err3 ~= "" then
                    redis_state.msg = "TTL error: " .. err3
                elseif ttl <= 0 then
                    redis_state.msg = "TTL should be > 0, got " .. tostring(ttl)
                else
                    redis_state.ok  = true
                    redis_state.msg = "ok (ttl=" .. tostring(ttl) .. ")"
                end
            end)
        end)
    end)
end

-- ── 7. AsyncExists ─────────────────────────────────────────────────
function test_redis_exists()
    reset_redis()
    local key = "db_test_lua_exists"
    redis.AsyncDel(key, function()
        redis.AsyncExists(key, function(err, exists)
            if err ~= "" then
                redis_state.msg  = "Exists error: " .. err
                redis_state.done = true
                return
            end
            if exists then
                redis_state.msg = "key should not exist after del"
                redis_state.done = true
                return
            end
            redis.AsyncSet(key, "x", function()
                redis.AsyncExists(key, function(err2, exists2)
                    redis_state.done = true
                    if err2 ~= "" then
                        redis_state.msg = "Exists2 error: " .. err2
                    elseif not exists2 then
                        redis_state.msg = "key should exist after set"
                    else
                        redis_state.ok  = true
                        redis_state.msg = "ok"
                    end
                end)
            end)
        end)
    end)
end

-- ── 8. AsyncLPush / AsyncLPop / AsyncLLen ──────────────────────────
function test_redis_list()
    reset_redis()
    local key = "db_test_lua_list"
    redis.AsyncDel(key, function()
        redis.AsyncLPush(key, "c", function()
            redis.AsyncLPush(key, "b", function()
                redis.AsyncLPush(key, "a", function(err, n)
                    if err ~= "" then
                        redis_state.msg  = "LPush error: " .. err
                        redis_state.done = true
                        return
                    end
                    redis.AsyncLLen(key, function(err2, len)
                        if err2 ~= "" then
                            redis_state.msg  = "LLen error: " .. err2
                            redis_state.done = true
                            return
                        end
                        if len ~= 3 then
                            redis_state.msg = "LLen expected 3, got " .. tostring(len)
                            redis_state.done = true
                            return
                        end
                        redis.AsyncLPop(key, function(err3, val)
                            redis_state.done = true
                            if err3 ~= "" then
                                redis_state.msg = "LPop error: " .. err3
                            elseif val ~= "a" then
                                redis_state.msg = "LPop expected 'a', got '" .. tostring(val) .. "'"
                            else
                                redis_state.ok  = true
                                redis_state.msg = "ok"
                            end
                        end)
                    end)
                end)
            end)
        end)
    end)
end

-- ── 9. AsyncZRevRank / AsyncZCount / AsyncZIncrBy / AsyncZRange / etc. ──
function test_redis_zset_adv()
    reset_redis()
    local key = "db_test_lua_zadv"

    -- Cleanup then add 4 members
    redis.AsyncDel(key, function()
        redis.AsyncZAdd(key, 10.0, "m_a", function()
            redis.AsyncZAdd(key, 20.0, "m_b", function()
                redis.AsyncZAdd(key, 30.0, "m_c", function()
                    redis.AsyncZAdd(key, 40.0, "m_d", function()

                        -- Step 1: ZRevRank
                        redis.AsyncZRevRank(key, "m_a", function(e1, r1)
                            if e1 ~= "" or r1 ~= 3 then
                                redis_state.msg  = string.format("ZRevRank m_a: got rank=%d", r1 or -1)
                                redis_state.done = true
                                return
                            end
                            redis.AsyncZRevRank(key, "m_d", function(e1b, r1b)
                                if e1b ~= "" or r1b ~= 0 then
                                    redis_state.msg  = string.format("ZRevRank m_d: got rank=%d", r1b or -1)
                                    redis_state.done = true
                                    return
                                end

                                -- Step 2: ZCount 10-30 → 3
                                redis.AsyncZCount(key, 10.0, 30.0, function(e2, c2)
                                    if e2 ~= "" or c2 ~= 3 then
                                        redis_state.msg  = string.format("ZCount 10-30: got %d", c2 or -1)
                                        redis_state.done = true
                                        return
                                    end

                                    -- Step 3: ZIncrBy m_a +5 → 15.0
                                    redis.AsyncZIncrBy(key, "m_a", 5.0, function(e3, s3)
                                        if e3 ~= "" or math.abs(s3 - 15.0) >= 0.001 then
                                            redis_state.msg  = string.format("ZIncrBy m_a+5: got %.1f", s3 or -1)
                                            redis_state.done = true
                                            return
                                        end

                                        -- Step 4: ZRange 0 -1 → 4 members
                                        redis.AsyncZRange(key, 0, -1, false, function(e4, m4)
                                            if e4 ~= "" or #m4 ~= 4 then
                                                redis_state.msg = string.format("ZRange 0 -1: got %d members", #(m4 or {}))
                                                redis_state.done = true
                                                return
                                            end

                                            -- Step 5: ZRange with scores → 8 elements
                                            redis.AsyncZRange(key, 0, -1, true, function(e5, m5)
                                                if e5 ~= "" or #m5 ~= 8 then
                                                    redis_state.msg = string.format("ZRange with_scores: got %d", #(m5 or {}))
                                                    redis_state.done = true
                                                    return
                                                end

                                                -- Step 6: ZRevRange → 4 reversed
                                                redis.AsyncZRevRange(key, 0, -1, false, function(e6, m6)
                                                    if e6 ~= "" or #m6 ~= 4 then
                                                        redis_state.msg = string.format("ZRevRange: got %d", #(m6 or {}))
                                                        redis_state.done = true
                                                        return
                                                    end

                                                    -- Step 7: ZRangeByScore 10-30 → 3
                                                    redis.AsyncZRangeByScore(key, 10.0, 30.0, false, function(e7, m7)
                                                        if e7 ~= "" or #m7 ~= 3 then
                                                            redis_state.msg = string.format("ZRangeByScore: got %d", #(m7 or {}))
                                                            redis_state.done = true
                                                            return
                                                        end

                                                        -- Step 8: ZRevRangeByScore 30-10 → 3 reversed
                                                        redis.AsyncZRevRangeByScore(key, 30.0, 10.0, false, function(e8, m8)
                                                            if e8 ~= "" or #m8 ~= 3 then
                                                                redis_state.msg = string.format("ZRevRangeByScore: got %d", #(m8 or {}))
                                                                redis_state.done = true
                                                                return
                                                            end

                                                            -- Step 9: ZRangeWithScores → 4 pairs
                                                            redis.AsyncZRangeWithScores(key, 0, -1, function(e9, p9)
                                                                if e9 ~= "" or #p9 ~= 4 or p9[1].member == nil or p9[1].score == nil then
                                                                    redis_state.msg = string.format("ZRangeWithScores: got %d pairs", #(p9 or {}))
                                                                    redis_state.done = true
                                                                    return
                                                                end

                                                                -- Step 10: ZRevRangeWithScores
                                                                redis.AsyncZRevRangeWithScores(key, 0, -1, function(e10, p10)
                                                                    if e10 ~= "" or #p10 ~= 4 or p10[1].member == nil then
                                                                        redis_state.msg = string.format("ZRevRangeWithScores: got %d", #(p10 or {}))
                                                                        redis_state.done = true
                                                                        return
                                                                    end

                                                                    -- Step 11: ZRemRangeByRank 0-1 → remove 2
                                                                    redis.AsyncZRemRangeByRank(key, 0, 1, function(e11, r11)
                                                                        if e11 ~= "" or r11 ~= 2 then
                                                                            redis_state.msg = string.format("ZRemRangeByRank: got %d", r11 or -1)
                                                                            redis_state.done = true
                                                                            return
                                                                        end

                                                                        -- Step 12: ZRemRangeByScore → remove rest
                                                                        redis.AsyncZRemRangeByScore(key, 10.0, 100.0, function(e12, r12)
                                                                            redis_state.done = true
                                                                            if e12 ~= "" then
                                                                                redis_state.msg = "ZRemRangeByScore error: " .. e12
                                                                            elseif r12 < 1 then
                                                                                redis_state.msg = string.format("ZRemRangeByScore: got %d", r12 or -1)
                                                                            else
                                                                                redis_state.ok  = true
                                                                                redis_state.msg = string.format("ok (all %d adv zset tests passed)", 12)
                                                                            end
                                                                        end)
                                                                    end)
                                                                end)
                                                            end)
                                                        end)
                                                    end)
                                                end)
                                            end)
                                        end)
                                    end)
                                end)
                            end)
                        end)
                    end)
                end)
            end)
        end)
    end)
end


-- ═══════════════════════════════════════════════════════════════════
-- PostgreSQL 测试
-- ═══════════════════════════════════════════════════════════════════

pg_state = { done = false, ok = false, msg = "" }

local function reset_pg()
    pg_state.done = false
    pg_state.ok   = false
    pg_state.msg  = ""
end

-- ── 9. AsyncConnect + AsyncQuery ────────────────────────────────────
function test_pg_connect_query()
    reset_pg()
    pg.AsyncConnect({
        host     = "192.168.31.186",
        port     = 5432,
        database = "mydb",
        user     = "fys",
        password = "fengyu",
        connect_timeout = 5
    }, function(err, ok)
        if err ~= "" then
            pg_state.msg  = "Connect error: " .. err
            pg_state.done = true
            return
        end
        if not ok then
            pg_state.msg  = "Connect returned false"
            pg_state.done = true
            return
        end

        -- 简单查询验证
        pg.AsyncQuery("SELECT 1 AS num", function(err2, rows)
            pg_state.done = true
            if err2 ~= "" then
                pg_state.msg = "Query error: " .. err2
            elseif rows == nil or #rows == 0 then
                pg_state.msg = "Query returned 0 rows"
            else
                pg_state.ok  = true
                pg_state.msg = "ok"
            end
        end)
    end)
end

-- ── 10. AsyncExecute (DDL + DML) ───────────────────────────────────
function test_pg_execute()
    reset_pg()

    -- 建表
    pg.AsyncExecute([[CREATE TABLE IF NOT EXISTS db_test_lua (
        id    SERIAL PRIMARY KEY,
        name  TEXT NOT NULL,
        score INTEGER DEFAULT 0
    )]], function(err, n)
        if err ~= "" then
            pg_state.msg  = "CREATE TABLE error: " .. err
            pg_state.done = true
            return
        end

        -- 插入两条
        pg.AsyncExecute("INSERT INTO db_test_lua (name, score) VALUES ($1, $2), ($3, $4)",
                        "alice", 100, "bob", 200, function(err2, n2)
            if err2 ~= "" then
                pg_state.msg  = "INSERT error: " .. err2
                pg_state.done = true
                return
            end
            if n2 ~= 2 then
                pg_state.msg = "INSERT affected " .. tostring(n2) .. " rows (expected 2)"
                pg_state.done = true
                return
            end

            -- 条件查询验证
            pg.AsyncQuery("SELECT name, score FROM db_test_lua WHERE score > $1 ORDER BY score", 150,
                function(err3, rows)
                    pg_state.done = true
                    if err3 ~= "" then
                        pg_state.msg = "SELECT error: " .. err3
                        return
                    end
                    if rows == nil or #rows ~= 1 then
                        pg_state.msg = "SELECT expected 1 row, got " .. tostring(#rows or 0)
                        return
                    end
                    pg_state.ok  = true
                    pg_state.msg = "ok"
                end)
        end)
    end)
end

-- ── 11. AsyncExecute (UPDATE) ──────────────────────────────────────
function test_pg_update()
    reset_pg()
    pg.AsyncExecute("UPDATE db_test_lua SET score = $1 WHERE name = $2",
                    999, "alice", function(err, n)
        pg_state.done = true
        if err ~= "" then
            pg_state.msg = "UPDATE error: " .. err
        elseif n ~= 1 then
            pg_state.msg = "UPDATE affected " .. tostring(n) .. " rows (expected 1)"
        else
            pg_state.ok  = true
            pg_state.msg = "ok"
        end
    end)
end

-- ── 12. AsyncExecute (DELETE) ──────────────────────────────────────
function test_pg_delete()
    reset_pg()
    pg.AsyncExecute("DELETE FROM db_test_lua WHERE name = $1", "bob", function(err, n)
        pg_state.done = true
        if err ~= "" then
            pg_state.msg = "DELETE error: " .. err
        elseif n ~= 1 then
            pg_state.msg = "DELETE affected " .. tostring(n) .. " rows (expected 1)"
        else
            pg_state.ok  = true
            pg_state.msg = "ok"
        end
    end)
end

-- ── 13. 清理 ───────────────────────────────────────────────────────
function test_pg_cleanup()
    reset_pg()
    pg.AsyncExecute("DROP TABLE IF EXISTS db_test_lua CASCADE", function(err, n)
        pg_state.done = true
        if err ~= "" then
            pg_state.msg = "DROP TABLE error: " .. err
        else
            pg_state.ok  = true
            pg_state.msg = "ok"
        end
    end)
end

-- ═══════════════════════════════════════════════════════════════════
-- 协程桥接 (Coroutine Bridge) 测试
--
-- 使用 C++ 提供的 redis.Await / pg.Await（见 register_redis.cpp / register_postgresql.cpp）
-- 将异步回调风格改写为同步协程风格。
-- ═══════════════════════════════════════════════════════════════════

-- ── Redis 协程测试 ─────────────────────────────────────────────────

function test_redis_ping_coro()
    reset_redis()
    local co = coroutine.create(function()
        local err, ok = redis.Await("Ping")
        redis_state.done = true
        if err ~= "" then
            redis_state.msg = "coro Ping error: " .. err
        elseif not ok then
            redis_state.msg = "coro Ping returned false"
        else
            redis_state.ok  = true
            redis_state.msg = "ok"
        end
    end)
    local stat, err2 = coroutine.resume(co)
    if not stat then
        redis_state.done = true
        redis_state.msg = "coro resume error: " .. tostring(err2)
    end
end

function test_redis_set_get_coro()
    reset_redis()
    local co = coroutine.create(function()
        local key = "db_test_lua_kv_coro"
        local val = "hello_coro_" .. math.random(10000, 99999)

        local err = redis.Await("Set", key, val)
        if err ~= "" then
            redis_state.msg  = "coro Set error: " .. err
            redis_state.done = true
            return
        end

        local err2, got = redis.Await("Get", key)
        redis_state.done = true
        if err2 ~= "" then
            redis_state.msg = "coro Get error: " .. err2
        elseif got ~= val then
            redis_state.msg = "coro value mismatch: got '" .. tostring(got) .. "'"
        else
            redis_state.ok  = true
            redis_state.msg = "ok"
        end
    end)
    coroutine.resume(co)
end

function test_redis_hash_coro()
    reset_redis()
    local co = coroutine.create(function()
        local key   = "db_test_lua_hash_coro"
        local field = "title"
        local val   = "hello_hash_coro"

        redis.Await("HDel", key, field)  -- 先删除确保干净
        local err = redis.Await("HSet", key, field, val)
        if err ~= "" then
            redis_state.msg  = "coro HSet error: " .. err
            redis_state.done = true
            return
        end

        local err2, hval = redis.Await("HGet", key, field)
        redis_state.done = true
        if err2 ~= "" then
            redis_state.msg = "coro HGet error: " .. err2
        elseif hval ~= val then
            redis_state.msg = "coro HGet mismatch: " .. tostring(hval)
        else
            redis_state.ok  = true
            redis_state.msg = "ok"
        end
    end)
    coroutine.resume(co)
end

function test_redis_incr_coro()
    reset_redis()
    local co = coroutine.create(function()
        local key = "db_test_lua_incr_coro"

        redis.Await("Del", key)
        local err, n = redis.Await("Incr", key)
        if err ~= "" then
            redis_state.msg  = "coro Incr error: " .. err
            redis_state.done = true
            return
        end
        if n ~= 1 then
            redis_state.msg = "coro Incr expected 1, got " .. tostring(n)
            redis_state.done = true
            return
        end

        local err2, n2 = redis.Await("IncrBy", key, 5)
        redis_state.done = true
        if err2 ~= "" then
            redis_state.msg = "coro IncrBy error: " .. err2
        elseif n2 ~= 6 then
            redis_state.msg = "coro IncrBy expected 6, got " .. tostring(n2)
        else
            redis_state.ok  = true
            redis_state.msg = "ok"
        end
    end)
    coroutine.resume(co)
end

function test_redis_expire_ttl_coro()
    reset_redis()
    local co = coroutine.create(function()
        local key = "db_test_lua_ttl_coro"

        local err = redis.Await("Set", key, "tmp")
        if err ~= "" then
            redis_state.msg  = "coro Set error: " .. err
            redis_state.done = true
            return
        end

        local err2 = redis.Await("Expire", key, 100)
        if err2 ~= "" then
            redis_state.msg  = "coro Expire error: " .. err2
            redis_state.done = true
            return
        end

        local err3, ttl = redis.Await("TTL", key)
        redis_state.done = true
        if err3 ~= "" then
            redis_state.msg = "coro TTL error: " .. err3
        elseif ttl <= 0 then
            redis_state.msg = "coro TTL should be > 0, got " .. tostring(ttl)
        else
            redis_state.ok  = true
            redis_state.msg = "ok (ttl=" .. tostring(ttl) .. ")"
        end
    end)
    coroutine.resume(co)
end

function test_redis_list_coro()
    reset_redis()
    local co = coroutine.create(function()
        local key = "db_test_lua_list_coro"

        redis.Await("Del", key)
        redis.Await("LPush", key, "c")
        redis.Await("LPush", key, "b")
        local err, n = redis.Await("LPush", key, "a")
        if err ~= "" then
            redis_state.msg  = "coro LPush error: " .. err
            redis_state.done = true
            return
        end

        local err2, len = redis.Await("LLen", key)
        if err2 ~= "" then
            redis_state.msg  = "coro LLen error: " .. err2
            redis_state.done = true
            return
        end
        if len ~= 3 then
            redis_state.msg = "coro LLen expected 3, got " .. tostring(len)
            redis_state.done = true
            return
        end

        local err3, val = redis.Await("LPop", key)
        redis_state.done = true
        if err3 ~= "" then
            redis_state.msg = "coro LPop error: " .. err3
        elseif val ~= "a" then
            redis_state.msg = "coro LPop expected 'a', got '" .. tostring(val) .. "'"
        else
            redis_state.ok  = true
            redis_state.msg = "ok"
        end
    end)
    coroutine.resume(co)
end

function test_redis_zset_adv_coro()
    reset_redis()
    local co = coroutine.create(function()
        local key = "db_test_lua_zadv_coro"

        redis.Await("Del", key)
        redis.Await("ZAdd", key, 10.0, "m_a")
        redis.Await("ZAdd", key, 20.0, "m_b")
        redis.Await("ZAdd", key, 30.0, "m_c")
        redis.Await("ZAdd", key, 40.0, "m_d")

        local e1, r1 = redis.Await("ZRevRank", key, "m_a")
        if e1 ~= "" or r1 ~= 3 then
            redis_state.msg  = "coro ZRevRank m_a: got " .. tostring(r1)
            redis_state.done = true
            return
        end

        local e2, c2 = redis.Await("ZCount", key, 10.0, 30.0)
        if e2 ~= "" or c2 ~= 3 then
            redis_state.msg  = "coro ZCount 10-30: got " .. tostring(c2)
            redis_state.done = true
            return
        end

        local e3, s3 = redis.Await("ZIncrBy", key, "m_a", 5.0)
        if e3 ~= "" or math.abs(s3 - 15.0) >= 0.001 then
            redis_state.msg  = "coro ZIncrBy m_a+5: got " .. tostring(s3)
            redis_state.done = true
            return
        end

        local e4, m4 = redis.Await("ZRange", key, 0, -1, false)
        if e4 ~= "" or #m4 ~= 4 then
            redis_state.msg  = "coro ZRange 0 -1: got " .. tostring(#(m4 or {}))
            redis_state.done = true
            return
        end

        local e5, m5 = redis.Await("ZRange", key, 0, -1, true)
        if e5 ~= "" or #m5 ~= 8 then
            redis_state.msg  = "coro ZRange with_scores: got " .. tostring(#(m5 or {}))
            redis_state.done = true
            return
        end

        local e6, m6 = redis.Await("ZRevRange", key, 0, -1, false)
        if e6 ~= "" or #m6 ~= 4 then
            redis_state.msg  = "coro ZRevRange: got " .. tostring(#(m6 or {}))
            redis_state.done = true
            return
        end

        local e7, m7 = redis.Await("ZRangeByScore", key, 10.0, 30.0, false)
        if e7 ~= "" or #m7 ~= 3 then
            redis_state.msg  = "coro ZRangeByScore: got " .. tostring(#(m7 or {}))
            redis_state.done = true
            return
        end

        local e8, m8 = redis.Await("ZRevRangeByScore", key, 30.0, 10.0, false)
        if e8 ~= "" or #m8 ~= 3 then
            redis_state.msg  = "coro ZRevRangeByScore: got " .. tostring(#(m8 or {}))
            redis_state.done = true
            return
        end

        local e9, p9 = redis.Await("ZRangeWithScores", key, 0, -1)
        if e9 ~= "" or #p9 ~= 4 or p9[1].member == nil or p9[1].score == nil then
            redis_state.msg  = "coro ZRangeWithScores: got " .. tostring(#(p9 or {}))
            redis_state.done = true
            return
        end

        local e10, p10 = redis.Await("ZRevRangeWithScores", key, 0, -1)
        if e10 ~= "" or #p10 ~= 4 or p10[1].member == nil then
            redis_state.msg  = "coro ZRevRangeWithScores: got " .. tostring(#(p10 or {}))
            redis_state.done = true
            return
        end

        local e11, r11 = redis.Await("ZRemRangeByRank", key, 0, 1)
        if e11 ~= "" or r11 ~= 2 then
            redis_state.msg  = "coro ZRemRangeByRank: got " .. tostring(r11)
            redis_state.done = true
            return
        end

        local e12, r12 = redis.Await("ZRemRangeByScore", key, 10.0, 100.0)
        redis_state.done = true
        if e12 ~= "" then
            redis_state.msg = "coro ZRemRangeByScore error: " .. e12
        elseif r12 < 1 then
            redis_state.msg = "coro ZRemRangeByScore: got " .. tostring(r12)
        else
            redis_state.ok  = true
            redis_state.msg = "ok"
        end
    end)
    coroutine.resume(co)
end


-- ── PostgreSQL 协程测试 ────────────────────────────────────────────

function test_pg_connect_query_coro()
    reset_pg()
    local co = coroutine.create(function()
        local err, ok = pg.Await("Connect", {
            host     = "192.168.31.186",
            port     = 5432,
            database = "mydb",
            user     = "fys",
            password = "fengyu",
            connect_timeout = 5
        })
        if err ~= "" then
            pg_state.msg  = "coro Connect error: " .. err
            pg_state.done = true
            return
        end
        if not ok then
            pg_state.msg  = "coro Connect returned false"
            pg_state.done = true
            return
        end

        local err2, rows = pg.Await("Query", "SELECT 1 AS num")
        pg_state.done = true
        if err2 ~= "" then
            pg_state.msg = "coro Query error: " .. err2
        elseif rows == nil or #rows == 0 then
            pg_state.msg = "coro Query returned 0 rows"
        else
            pg_state.ok  = true
            pg_state.msg = "ok"
        end
    end)
    coroutine.resume(co)
end

function test_pg_execute_coro()
    reset_pg()
    local co = coroutine.create(function()

        local err, n = pg.Await("Execute", [[CREATE TABLE IF NOT EXISTS db_test_lua_coro (
            id    SERIAL PRIMARY KEY,
            name  TEXT NOT NULL,
            score INTEGER DEFAULT 0
        )]])
        if err ~= "" then
            pg_state.msg  = "coro CREATE TABLE error: " .. err
            pg_state.done = true
            return
        end

        local err2, n2 = pg.Await("Execute",
            "INSERT INTO db_test_lua_coro (name, score) VALUES ($1, $2), ($3, $4)",
            "alice", 100, "bob", 200)
        if err2 ~= "" then
            pg_state.msg  = "coro INSERT error: " .. err2
            pg_state.done = true
            return
        end
        if n2 ~= 2 then
            pg_state.msg = "coro INSERT affected " .. tostring(n2) .. " rows (expected 2)"
            pg_state.done = true
            return
        end

        local err3, rows = pg.Await("Query",
            "SELECT name, score FROM db_test_lua_coro WHERE score > $1 ORDER BY score", 150)
        pg_state.done = true
        if err3 ~= "" then
            pg_state.msg = "coro SELECT error: " .. err3
            return
        end
        if rows == nil or #rows ~= 1 then
            pg_state.msg = "coro SELECT expected 1 row, got " .. tostring(#rows or 0)
            return
        end
        pg_state.ok  = true
        pg_state.msg = "ok"
    end)
    coroutine.resume(co)
end

function test_pg_update_coro()
    reset_pg()
    local co = coroutine.create(function()
        local err, n = pg.Await("Execute", "UPDATE db_test_lua_coro SET score = $1 WHERE name = $2",
                                999, "alice")
        pg_state.done = true
        if err ~= "" then
            pg_state.msg = "coro UPDATE error: " .. err
        elseif n ~= 1 then
            pg_state.msg = "coro UPDATE affected " .. tostring(n) .. " rows (expected 1)"
        else
            pg_state.ok  = true
            pg_state.msg = "ok"
        end
    end)
    coroutine.resume(co)
end

function test_pg_delete_coro()
    reset_pg()
    local co = coroutine.create(function()
        local err, n = pg.Await("Execute", "DELETE FROM db_test_lua_coro WHERE name = $1", "bob")
        pg_state.done = true
        if err ~= "" then
            pg_state.msg = "coro DELETE error: " .. err
        elseif n ~= 1 then
            pg_state.msg = "coro DELETE affected " .. tostring(n) .. " rows (expected 1)"
        else
            pg_state.ok  = true
            pg_state.msg = "ok"
        end
    end)
    coroutine.resume(co)
end

function test_pg_cleanup_coro()
    reset_pg()
    local co = coroutine.create(function()
        local err, n = pg.Await("Execute", "DROP TABLE IF EXISTS db_test_lua_coro CASCADE")
        pg_state.done = true
        if err ~= "" then
            pg_state.msg = "coro DROP TABLE error: " .. err
        else
            pg_state.ok  = true
            pg_state.msg = "ok"
        end
    end)
    coroutine.resume(co)
end


-- ═══════════════════════════════════════════════════════════════════
print("db_test_lua_redis_pg.lua loaded")
