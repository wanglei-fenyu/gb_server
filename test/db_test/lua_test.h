#pragma once

/// Lua Redis/PG 绑定测试 (通过 WorkerManager 初始化生产绑定)
/// 测试实际的 register_redis / register_postgresql 绑定，
/// 使用 Worker::Post / ProcessFrame 异步回调桥接。
int MenuTestLuaScriptBindings();
