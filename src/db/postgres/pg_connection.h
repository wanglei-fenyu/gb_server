#pragma once
#include "db_connection.h"
#include "db_config.h"
#include "db_result.h"
#include "async_simple/Future.h"
#include "async_simple/Promise.h"
#include <libpq-fe.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <functional>
#include <memory>
#include <queue>
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

    // ── 异步回调 API（供 Lua 桥接使用，基于 libpq 非阻塞模式 + Asio reactor） ──

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
    // ── 异步管道（libpq 非阻塞模式 + Asio reactor） ──────────────────────

    struct AsyncOp;

    /// 启动一次异步操作（PQsendQuery / PQsendQueryParams）。
    /// send_fn 在 IO 线程上调用，返回 true 表示发送成功。
    void StartSend(std::function<bool(PGconn*)> send_fn,
                   std::function<void(DbResult)> callback);

    /// 继续异步管道：flush → consume → getResult 循环。
    void ContinueOp(std::shared_ptr<AsyncOp> op);

    /// 将 PQsocket() 包装为 Asio tcp::socket（不转移所有权）。
    std::shared_ptr<boost::asio::ip::tcp::socket> WrapPgSocket();

    /// 完成当前操作，启动队列中下一个。
    void StartNext();

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

    /// 操作队列（确保同一连接上串行执行）。
    std::queue<std::shared_ptr<AsyncOp>> pending_ops_;
    /// 当前是否有操作在进行。
    bool                                 op_active_ = false;
};

} // namespace gb
