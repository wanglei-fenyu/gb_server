module;

#include "log/log.h"

module db.redis;

RedisConnectionPool::~RedisConnectionPool()
{
    CloseAll();
}

bool RedisConnectionPool::Init(const RedisConfig& cfg)
{
    config_ = cfg;
    connections_.clear();

    int pool_size = (cfg.pool_size > 0) ? cfg.pool_size : 1;

    for (int i = 0; i < pool_size; ++i)
    {
        auto conn = std::make_unique<RedisConnection>();
        bool ok   = conn->Connect(cfg);

        if (!ok)
        {
            LOG_WARN("Redis pool connection [{}/{}] failed: {}:{}",
                     i + 1, pool_size, cfg.host, cfg.port);
        }
        else
        {
            LOG_INFO("Redis pool connection [{}/{}] connected: {}:{}/{}",
                     i + 1, pool_size, cfg.host, cfg.port, cfg.db_index);
        }
        connections_.emplace_back(std::move(conn));
    }

    LOG_INFO("RedisConnectionPool initialized: {} connection(s), {} healthy",
             pool_size, CountHealthy());

    return !connections_.empty();
}

void RedisConnectionPool::CloseAll()
{
    for (auto& conn : connections_)
    {
        if (conn)
            conn->Disconnect();
    }
    connections_.clear();
    LOG_INFO("RedisConnectionPool closed all connections");
}

RedisConnection* RedisConnectionPool::GetConnection()
{
    if (connections_.empty())
        return nullptr;

    size_t size = connections_.size();
    // round-robin: 每次取下一个索引
    size_t idx = next_index_.fetch_add(1, std::memory_order_relaxed) % size;
    auto*  conn = connections_[idx].get();

    if (conn && conn->IsConnected())
        return conn;

    // 当前连接断开，尝试其他连接
    for (size_t i = 0; i < size; ++i)
    {
        size_t try_idx = (idx + 1 + i) % size;
        auto*  try_conn = connections_[try_idx].get();
        if (try_conn && try_conn->IsConnected())
            return try_conn;
    }

    return nullptr;
}

bool RedisConnectionPool::IsHealthy() const
{
    return CountHealthy() > 0;
}

int RedisConnectionPool::CountHealthy() const
{
    int count = 0;
    for (auto& conn : connections_)
    {
        if (conn && conn->IsConnected())
            ++count;
    }
    return count;
}
