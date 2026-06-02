#pragma once
#include <cstdint>
#include <string>

namespace gb
{

enum class DbType : uint8_t
{
    POSTGRESQL
};

/// 数据库连接配置。
struct DbConfig
{
    std::string host     = "127.0.0.1";
    uint16_t    port     = 0;          // 0 = 使用默认端口
    std::string user;
    std::string password;
    std::string database;
    bool        use_ssl  = false;

    int connect_timeout_sec = 5;
    int query_timeout_sec   = 10;

    size_t min_pool_size = 4;
    size_t max_pool_size = 32;

    /// 默认端口：PG=5432
    static uint16_t DefaultPort(DbType /*type*/) noexcept
    {
        return 5432;
    }
};

} // namespace gb
