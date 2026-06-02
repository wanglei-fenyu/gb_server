#pragma once
#include "db_connection.h"
#include "db_config.h"
#include "db_result.h"
#include "async_simple/Future.h"
#include "async_simple/Promise.h"
#include <libpq-fe.h>
#include <boost/asio/io_context.hpp>
#include <functional>
#include <memory>
#include <string>

namespace gb
{

/// PostgreSQL 异步连接（libpq 非阻塞模式 + Boost.Asio reactor）。
///
/// 线程安全说明：
///   - 所有异步方法（Connect/Query/Execute）必须在调用者协程上执行，
///     内部会切换到 IO 线程执行 libpq 操作。
///   - 析构函数线程安全。
///
class PgConnection final : public DbConnection
{
public:
    explicit PgConnection(boost::asio::io_context& io_ctx);
    ~PgConnection() override;

    // 禁用拷贝
    PgConnection(const PgConnection&) = delete;
    PgConnection& operator=(const PgConnection&) = delete;

    // ── DbConnection 接口 ─────────────────────────────────────────────────

    DbType type() const noexcept override { return DbType::POSTGRESQL; }

    async_simple::coro::Lazy<bool>          Connect(const DbConfig& cfg) override;
    async_simple::coro::Lazy<void>          Close() override;
    bool                                    IsConnected() const override;
    async_simple::coro::Lazy<bool>          Reset() override;

    // ── 同步接口（供连接池在 IO 线程上使用） ──────────────────────────────

    /// 同步关闭（线程安全）。
    void CloseSync();

    /// 同步连接（在 IO 线程上直接阻塞调用，不经过 Promise/Future）。
    bool ConnectSync(const DbConfig& cfg);

    async_simple::coro::Lazy<DbResult>      Query(std::string_view sql) override;
    async_simple::coro::Lazy<DbResult>      Query(
        std::string_view sql, const std::vector<DbValue>& params) override;
    async_simple::coro::Lazy<uint64_t>      Execute(std::string_view sql) override;

    async_simple::coro::Lazy<void>          Begin() override;
    async_simple::coro::Lazy<void>          Commit() override;
    async_simple::coro::Lazy<void>          Rollback() override;

    // ── 异步回调 API（供 Lua 桥接使用，在 PG IO 线程上执行同步操作后回调） ──

    /// 异步连接，完成后回调 ok(bool)。
    void AsyncConnect(const DbConfig& cfg, std::function<void(bool)> callback);

    /// 异步关闭，完成后回调。
    void AsyncClose(std::function<void()> callback);

    /// 异步查询，无参数。
    void AsyncQuery(std::string_view sql, std::function<void(DbResult)> callback);

    /// 异步参数化查询。
    void AsyncQuery(std::string_view sql, const std::vector<DbValue>& params,
                    std::function<void(DbResult)> callback);

    /// 异步 DML 执行。
    void AsyncExecute(std::string_view sql, std::function<void(uint64_t)> callback);

    /// 异步 BEGIN。
    void AsyncBegin(std::function<void(bool)> callback);

    /// 异步 COMMIT。
    void AsyncCommit(std::function<void(bool)> callback);

    /// 异步 ROLLBACK。
    void AsyncRollback(std::function<void(bool)> callback);

private:
    // ── 同步操作（在 IO 线程上执行） ──────────────────────────────────────

    /// 同步查询。
    DbResult SyncQuery(const char* sql);

    /// 参数化同步查询。
    DbResult SyncExecParams(const char* sql, const std::vector<DbValue>& params);

    /// 同步命令（BEGIN/COMMIT/ROLLBACK）。
    bool SyncCommand(const char* sql);

    /// 将 PGresult 解析为 DbResult。
    static DbResult ParseResult(PGresult* res);
    /// 将 libpq 文本值转为 DbValue。
    static DbValue ToDbValue(const char* value, Oid type_oid);
    /// 构建连接字符串。
    std::string BuildConnString(const DbConfig& cfg);

private:
    boost::asio::io_context&           io_ctx_;
    PGconn*                            conn_        = nullptr;
    DbConfig                           config_;
    bool                               connected_   = false;
};

} // namespace gb
