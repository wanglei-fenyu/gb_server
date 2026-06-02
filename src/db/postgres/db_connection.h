#pragma once
#include "db_config.h"
#include "db_result.h"
#include "db_value.h"
#include "async_simple/coro/Lazy.h"
#include <memory>
#include <string_view>

namespace gb
{

/// 异步数据库连接抽象接口。
///
/// 所有数据库操作都返回 async_simple::coro::Lazy<T>，可与项目的 CoRpc
/// 和 WorkerExecutor 无缝配合。
///
/// 实现类：
///   - PgConnection   (src/db/postgres/pg_connection.h)
///
class DbConnection : public std::enable_shared_from_this<DbConnection>
{
public:
    virtual ~DbConnection() = default;

    // ── 类型 ──
    virtual DbType type() const noexcept = 0;

    // ── 生命周期 ───────────────────────────────────────────────────────────

    /// 异步连接数据库。返回 true 表示成功。
    virtual async_simple::coro::Lazy<bool> Connect(const DbConfig& cfg) = 0;

    /// 异步关闭连接。
    virtual async_simple::coro::Lazy<void> Close() = 0;

    /// 是否已连接。
    virtual bool IsConnected() const = 0;

    /// 重置连接（断线重连时调用）。
    virtual async_simple::coro::Lazy<bool> Reset() = 0;

    // ── 查询 ───────────────────────────────────────────────────────────────

    /// 执行 SQL 并返回结果集。
    virtual async_simple::coro::Lazy<DbResult> Query(std::string_view sql) = 0;

    /// 参数化查询（防 SQL 注入）。
    /// 参数占位符：PG 用 $1, $2...
    virtual async_simple::coro::Lazy<DbResult> Query(
        std::string_view sql, const std::vector<DbValue>& params) = 0;

    /// 执行 DML（INSERT/UPDATE/DELETE），返回影响行数。
    virtual async_simple::coro::Lazy<uint64_t> Execute(std::string_view sql) = 0;

    // ── 事务 ───────────────────────────────────────────────────────────────

    virtual async_simple::coro::Lazy<void> Begin()   = 0;
    virtual async_simple::coro::Lazy<void> Commit()  = 0;
    virtual async_simple::coro::Lazy<void> Rollback() = 0;

    // ── 工具 ───────────────────────────────────────────────────────────────

    /// 生成 DbConnection 实例的工厂辅助。
    template <typename T, typename... Args>
    static std::shared_ptr<T> Create(Args&&... args)
    {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }
};

/// DbConnection 智能指针。
using DbConnectionPtr = std::shared_ptr<DbConnection>;

} // namespace gb
