#pragma once
#include "redis_config.h"
#include "redis_connection.h"
#include <memory>
#include <vector>
#include <atomic>

/// Redis 连接池（线程安全，round-robin）。
///
/// 使用方式：
/// @code
///   RedisConnectionPool pool;
///   pool.Init(config);
///   auto* conn = pool.GetConnection();
///   conn->Set("key", "value");
///   auto val = conn->Get("key");
/// @endcode
///
class RedisConnectionPool
{
public:
    RedisConnectionPool()  = default;
    ~RedisConnectionPool();

    // 禁用拷贝
    RedisConnectionPool(const RedisConnectionPool&) = delete;
    RedisConnectionPool& operator=(const RedisConnectionPool&) = delete;

    /// 初始化连接池。创建 pool_size 条连接并全部连接。
    /// @return true 表示全部连接成功（允许部分失败）。
    bool Init(const RedisConfig& cfg);

    /// 关闭所有连接并清空池。
    void CloseAll();

    /// 获取一条连接（round-robin）。
    /// @return 有效连接指针，或 nullptr（池为空 / 所有连接断开）。
    RedisConnection* GetConnection();

    /// 检查连接池状态。
    bool IsHealthy() const;

    /// 返回连接总数。
    int Size() const { return static_cast<int>(connections_.size()); }

    /// 返回健康（已连接）的连接数。
    int CountHealthy() const;

private:
    std::vector<std::unique_ptr<RedisConnection>> connections_;
    std::atomic<size_t>                           next_index_{0};
    RedisConfig                                   config_;
};
