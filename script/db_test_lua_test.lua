-- ═══════════════════════════════════════════════════════════════════
-- db_test Lua Script Binding Tests
-- 
-- 独立测试脚本，不依赖 Worker/NetworkManager。
-- 由 lua_test.cpp 加载，每个函数返回 (ok, detail) 给 C++。
-- ═══════════════════════════════════════════════════════════════════

-- ── 1: 纯 Lua 函数，C++ 调用并校验返回值 ──
function lua_meaning_of_life()
    return 42
end

-- ── 2: Lua 调用 C++ 注册函数 cpp_add ──
function lua_test_cpp_add()
    local r = cpp_add(10, 32)
    if r == 42 then return true, "10+32=42" end
    return false, "got " .. tostring(r)
end

-- ── 3: Lua 调用 C++ 注册函数 cpp_concat ──
function lua_test_cpp_concat()
    local r = cpp_concat("hello_", "world")
    if r == "hello_world" then return true, r end
    return false, "got " .. tostring(r)
end

-- ── 4: msgpack 往返 (pack → unpack) ──
function lua_test_msgpack_roundtrip()
    local data = msgpack.pack(42, "hello", 3.14)
    local a, b, c = msgpack.unpack(data)
    if a ~= 42 then return false, "int mismatch: " .. tostring(a) end
    if b ~= "hello" then return false, "str mismatch: " .. tostring(b) end
    if math.abs(c - 3.14) >= 0.001 then return false, "float mismatch: " .. tostring(c) end
    return true, "42, hello, 3.14"
end

-- ── 5: msgpack 键值对 ──
function lua_test_msgpack_kv()
    local data = msgpack.pack("answer", 42)
    local k, v = msgpack.unpack(data)
    if k ~= "answer" then return false, "key mismatch: " .. tostring(k) end
    if v ~= 42 then return false, "val mismatch: " .. tostring(v) end
    return true, "answer=42"
end

-- ── 6: Lua 调用 log.Info / Warning / Error ──
function lua_test_log()
    log.Info("lua_binding: info from Lua")
    log.Warning("lua_binding: warning from Lua")
    log.Error("lua_binding: error from Lua")
    return true, "log output written to db_test.log"
end

-- ── 7: C++ 用 msgpack pack 数据 → Lua unpack 验证 ──
--   lua_test_cpp_packed(data) 接收 C++ pack 好的二进制数据
function lua_test_cpp_packed(data)
    local ok, a, b = pcall(msgpack.unpack, data)
    if not ok then return false, "unpack failed: " .. tostring(a) end
    if a ~= 100 then return false, "first value mismatch: " .. tostring(a) end
    if b ~= "from_cpp" then return false, "second value mismatch: " .. tostring(b) end
    return true, "100, from_cpp"
end
