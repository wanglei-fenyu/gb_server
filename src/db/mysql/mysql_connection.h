#pragma once
#include "db/db_connection.h"
#include "db/db_config.h"
#include "db/db_result.h"
#include "async_simple/coro/Lazy.h"
#include "async_simple/Future.h"
#include "async_simple/Promise.h"
#include <boost/mysql/error_with_diagnostics.hpp>
#include <boost/mysql/handshake_params.hpp>
#include <boost/mysql/results.hpp>
#include <boost/mysql/tcp_ssl.hpp>
#include <boost/asio/ssl/context.hpp>
#include <memory>
#include <string>

namespace gb
{

/// MySQL 异步连接（boost/mysql 同步操作 + IO 线程桥接）。
class MySqlConnection final : public DbConnection
{
public:
    explicit MySqlConnection(boost::asio::io_context& io_ctx);
    ~MySqlConnection() override;

    MySqlConnection(const MySqlConnection&) = delete;
    MySqlConnection& operator=(const MySqlConnection&) = delete;

    // ── DbConnection 接口 ─────────────────────────────────────────────────

    DbType type() const noexcept override { return DbType::MYSQL; }

    async_simple::coro::Lazy<bool>          Connect(const DbConfig& cfg) override;
    async_simple::coro::Lazy<void>          Close() override;
    async_simple::coro::Lazy<bool>          Reset() override;

    bool IsConnected() const override;
    void CloseSync() override;

    async_simple::coro::Lazy<DbResult>      Query(std::string_view sql) override;
    async_simple::coro::Lazy<DbResult>      Query(
        std::string_view sql, const std::vector<DbValue>& params) override;
    async_simple::coro::Lazy<uint64_t>      Execute(std::string_view sql) override;

    async_simple::coro::Lazy<void>          Begin() override;
    async_simple::coro::Lazy<void>          Commit() override;
    async_simple::coro::Lazy<void>          Rollback() override;

    // ── 同步接口（供连接池使用） ──────────────────────────────────────────

    bool ConnectSync(const DbConfig& cfg);

private:
    // ── 同步操作（在 IO 线程上执行） ──────────────────────────────────────

    DbResult SyncQuery(const char* sql);

    /// 将 boost::mysql::results 转换为 DbResult。
    static DbResult ParseResult(const boost::mysql::results& res);

    /// 将 boost::mysql::field_view 转换为 DbValue。
    static DbValue ToDbValue(const boost::mysql::field_view& field);

private:
    boost::asio::io_context&           io_ctx_;
    boost::asio::ssl::context          ssl_ctx_{boost::asio::ssl::context::tls_client};
    boost::mysql::tcp_ssl_connection   conn_;
    DbConfig                           config_;
    bool                               connected_ = false;
};

} // namespace gb
