#pragma once

/// Redis 选项测试 (独立工程, 不依赖 App/Worker/Lua)
/// 每个测试创建自己的 RedisConnection, 运行完后销毁。
/// 数据保留在 Redis 中，方便后续检查。
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
