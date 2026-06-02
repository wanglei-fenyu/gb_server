#pragma once
#include "script/script.h"

/// 注册 Redis Lua API（在 _lua_() 中调用）。
///
/// 注册后 Lua 侧可使用全局 redis 表的异步 API：
/// @code
///   redis.Connect({host="127.0.0.1", port=6379})  -- 同步初始化连接池
///
///   -- 异步回调方式：
///   redis.AsyncSet("key", "value", function(err)
///       redis.AsyncGet("key", function(err2, val)
///           log.Info("got: " .. val)
///       end)
///   end)
///
///   -- 或使用 Lua 协程桥接：
///   local co = coroutine.create(function()
///       local err = redis.Await("Set", "key", "value")
///       local err2, val = redis.Await("Get", "key")
///       log.Info(val)
///   end)
///   coroutine.resume(co)
/// @endcode
/// 所有异步回调约定：callback(err, ...)，err="" 表示成功。
void register_redis(std::shared_ptr<Script>& scriptPtr);
