#pragma once
#include "script/script.h"

/// 注册 PostgreSQL Lua API（在 _lua_() 中调用）。
///
/// 注册后 Lua 侧可使用全局 pg 表的异步 API：
/// @code
///   pg.AsyncConnect({host="127.0.0.1", port=5432, user="postgres",
///                    password="123456", database="mydb"}, function(err, ok)
///       if ok then
///           pg.AsyncQuery("SELECT * FROM users", function(err, rows)
///               for _, row in ipairs(rows) do log.Info("name: " .. row.name) end
///           end)
///           pg.AsyncExecute("DELETE FROM users WHERE id = $1", 1, function(err, n)
///               log.Info("deleted: " .. n)
///           end)
///       end
///   end)
///
///   -- 或使用 Lua 协程桥接：
///   local co = coroutine.create(function()
///       local err, rows = pg.Await("Query", "SELECT * FROM users")
///       ...
///   end)
///   coroutine.resume(co)
/// @endcode
/// 所有异步回调约定：callback(err, ...)，err="" 表示成功。
void register_postgresql(std::shared_ptr<Script>& scriptPtr);
