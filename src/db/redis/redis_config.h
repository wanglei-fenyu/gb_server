#pragma once
#include <cstdint>
#include <string>

/// Redis 连接配置。
struct RedisConfig {
    std::string host         = "127.0.0.1";
    uint16_t    port         = 6379;
    std::string password;
    int         db_index     = 0;      ///< 选库编号，-1 表示不执行 SELECT
    int         pool_size    = 4;      ///< 连接池大小
    int         timeout_ms   = 5000;   ///< 读写超时（毫秒）
};
