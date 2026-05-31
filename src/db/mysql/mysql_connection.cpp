#include "mysql_connection.h"
#include "log/log.h"
#include "async_simple/coro/FutureAwaiter.h"
#include <boost/asio/post.hpp>
#include <boost/asio/ip/address.hpp>
#include <sstream>

namespace gb
{

// 前向声明
static std::string ValueToSqlString(const DbValue& v);

MySqlConnection::MySqlConnection(boost::asio::io_context& io_ctx)
    : io_ctx_(io_ctx)
    , conn_(io_ctx.get_executor(), ssl_ctx_)
{
}

MySqlConnection::~MySqlConnection()
{
    CloseSync();
}

// =========================================================================
// DbConnection 接口
// =========================================================================

async_simple::coro::Lazy<bool> MySqlConnection::Connect(const DbConfig& cfg)
{
    async_simple::Promise<bool> promise;
    auto future = promise.getFuture();

    auto self = shared_from_this();
    boost::asio::post(io_ctx_, [this, self, cfg,
                                promise = std::move(promise)]() mutable {
        promise.setValue(ConnectSync(cfg));
    });

    co_return co_await std::move(future);
}

async_simple::coro::Lazy<void> MySqlConnection::Close()
{
    async_simple::Promise<void> promise;
    auto future = promise.getFuture();

    auto self = shared_from_this();
    boost::asio::post(io_ctx_, [this, self, promise = std::move(promise)]() mutable {
        CloseSync();
        promise.setValue();
    });

    co_await std::move(future);
}

async_simple::coro::Lazy<bool> MySqlConnection::Reset()
{
    async_simple::Promise<bool> promise;
    auto future = promise.getFuture();

    auto self = shared_from_this();
    boost::asio::post(io_ctx_, [this, self, config_,
                                promise = std::move(promise)]() mutable {
        CloseSync();
        promise.setValue(ConnectSync(config_));
    });

    co_return co_await std::move(future);
}

bool MySqlConnection::IsConnected() const
{
    return connected_ && conn_.is_open();
}

void MySqlConnection::CloseSync()
{
    connected_ = false;
    if (conn_.is_open()) {
        boost::system::error_code ec;
        conn_.close(ec);
        if (ec) {
            LOG_WARN("[MySqlConnection] close error: {}", ec.message());
        }
    }
}

// =========================================================================
// 查询
// =========================================================================

async_simple::coro::Lazy<DbResult> MySqlConnection::Query(std::string_view sql)
{
    async_simple::Promise<DbResult> promise;
    auto future = promise.getFuture();

    auto self = shared_from_this();
    std::string sql_str(sql);
    boost::asio::post(io_ctx_, [this, self, sql = std::move(sql_str),
                                promise = std::move(promise)]() mutable {
        promise.setValue(SyncQuery(sql.c_str()));
    });

    co_return co_await std::move(future);
}

async_simple::coro::Lazy<DbResult> MySqlConnection::Query(
    std::string_view sql, const std::vector<DbValue>& params)
{
    // 简化实现：将参数拼接到 SQL 中（后续可改为 prepared statement）
    std::string expanded;
    if (params.empty()) {
        expanded = sql;
    } else {
        // 替换 ? 为参数值
        expanded.reserve(sql.size() + params.size() * 16);
        size_t pi = 0;
        for (size_t i = 0; i < sql.size(); ++i) {
            if (sql[i] == '?' && pi < params.size()) {
                expanded += ValueToSqlString(params[pi]);
                ++pi;
            } else {
                expanded += sql[i];
            }
        }
    }

    async_simple::Promise<DbResult> promise;
    auto future = promise.getFuture();

    auto self = shared_from_this();
    boost::asio::post(io_ctx_, [this, self, sql = std::move(expanded),
                                promise = std::move(promise)]() mutable {
        promise.setValue(SyncQuery(sql.c_str()));
    });

    co_return co_await std::move(future);
}

async_simple::coro::Lazy<uint64_t> MySqlConnection::Execute(std::string_view sql)
{
    async_simple::Promise<uint64_t> promise;
    auto future = promise.getFuture();

    auto self = shared_from_this();
    std::string sql_str(sql);
    boost::asio::post(io_ctx_, [this, self, sql = std::move(sql_str),
                                promise = std::move(promise)]() mutable {
        DbResult r = SyncQuery(sql.c_str());
        promise.setValue(r.is_ok() ? r.affected_rows() : 0);
    });

    co_return co_await std::move(future);
}

// =========================================================================
// 事务
// =========================================================================

async_simple::coro::Lazy<void> MySqlConnection::Begin()
{
    async_simple::Promise<void> promise;
    auto future = promise.getFuture();
    auto self = shared_from_this();

    boost::asio::post(io_ctx_, [this, self, promise = std::move(promise)]() mutable {
        SyncQuery("BEGIN");
        promise.setValue();
    });

    co_await std::move(future);
}

async_simple::coro::Lazy<void> MySqlConnection::Commit()
{
    async_simple::Promise<void> promise;
    auto future = promise.getFuture();
    auto self = shared_from_this();

    boost::asio::post(io_ctx_, [this, self, promise = std::move(promise)]() mutable {
        SyncQuery("COMMIT");
        promise.setValue();
    });

    co_await std::move(future);
}

async_simple::coro::Lazy<void> MySqlConnection::Rollback()
{
    async_simple::Promise<void> promise;
    auto future = promise.getFuture();
    auto self = shared_from_this();

    boost::asio::post(io_ctx_, [this, self, promise = std::move(promise)]() mutable {
        SyncQuery("ROLLBACK");
        promise.setValue();
    });

    co_await std::move(future);
}

// =========================================================================
// 同步接口
// =========================================================================

bool MySqlConnection::ConnectSync(const DbConfig& cfg)
{
    config_ = cfg;
    try {
        auto endpoint = Asio::ip::tcp::endpoint(
            Asio::ip::make_address(cfg.host.c_str()),
            cfg.port > 0 ? cfg.port : DbConfig::DefaultPort(DbType::MYSQL));
        boost::mysql::handshake_params params(
            cfg.user, cfg.password, cfg.database);
        conn_.connect(endpoint, params);
        connected_ = true;
        LOG_INFO("[MySqlConnection] connected to {}:{}/{}",
                 cfg.host, cfg.port, cfg.database);
        return true;
    } catch (const boost::mysql::error_with_diagnostics& e) {
        LOG_ERROR("[MySqlConnection] connect failed: {}", e.what());
        connected_ = false;
        return false;
    } catch (const std::exception& e) {
        LOG_ERROR("[MySqlConnection] connect exception: {}", e.what());
        connected_ = false;
        return false;
    }
}

// =========================================================================
// 内部
// =========================================================================

DbResult MySqlConnection::SyncQuery(const char* sql)
{
    if (!connected_ || !conn_.is_open()) {
        return DbResult::Error("Connection not available");
    }

    try {
        boost::mysql::results res;
        conn_.execute(sql, res);
        return ParseResult(res);
    } catch (const boost::mysql::error_with_diagnostics& e) {
        return DbResult::Error(e.what());
    } catch (const std::exception& e) {
        return DbResult::Error(e.what());
    }
}

std::string ValueToSqlString(const DbValue& v)
{
    if (v.is_null())
        return "NULL";
    switch (v.type()) {
        case DbFieldType::BOOL:
            return v.as_bool() ? "TRUE" : "FALSE";
        case DbFieldType::INT8:
            return std::to_string(v.as_int8());
        case DbFieldType::INT16:
            return std::to_string(v.as_int16());
        case DbFieldType::INT32:
            return std::to_string(v.as_int32());
        case DbFieldType::INT64:
            return std::to_string(v.as_int64());
        case DbFieldType::UINT64:
            return std::to_string(v.as_uint64());
        case DbFieldType::FLOAT:
            return std::to_string(v.as_float());
        case DbFieldType::DOUBLE:
            return std::to_string(v.as_double());
        case DbFieldType::STRING: {
            // 简单的转义（仅处理单引号和反斜杠）
            auto sv = v.as_string();
            std::string escaped;
            escaped.reserve(sv.size() + 4);
            escaped.push_back('\'');
            for (char ch : sv) {
                if (ch == '\'') escaped.append("''");
                else if (ch == '\\') escaped.append("\\\\");
                else escaped.push_back(ch);
            }
            escaped.push_back('\'');
            return escaped;
        }
        default:
            return "NULL";
    }
}

DbResult MySqlConnection::ParseResult(const boost::mysql::results& res)
{
    // 检查是否有结果集
    if (res.has_value()) {
        auto rows = res.rows();
        int n_cols = static_cast<int>(res.meta().size());

        std::vector<DbRow> result_rows;
        result_rows.reserve(rows.size());

        for (const auto& row : rows) {
            std::vector<DbField> fields;
            fields.reserve(static_cast<size_t>(n_cols));
            for (int c = 0; c < n_cols; ++c) {
                std::string name(res.meta()[c].field_name());
                fields.emplace_back(std::move(name), ToDbValue(row[c]));
            }
            result_rows.emplace_back(std::move(fields));
        }
        return DbResult::Rows(std::move(result_rows),
                              static_cast<uint32_t>(n_cols));
    }

    // DML
    uint64_t affected = res.affected_rows();
    uint64_t insert_id = 0;
    if (res.last_insert_id().has_value()) {
        insert_id = res.last_insert_id().value();
    }
    return DbResult::AffectedRows(affected, insert_id);
}

DbValue MySqlConnection::ToDbValue(const boost::mysql::field_view& field)
{
    if (field.is_null())
        return DbValue(nullptr);

    using boost::mysql::field_kind;
    switch (field.kind()) {
        case field_kind::int64:
            return DbValue(field.as_int64());
        case field_kind::uint64:
            return DbValue(static_cast<int64_t>(field.as_uint64()));
        case field_kind::float_:
            return DbValue(field.as_float());
        case field_kind::double_:
            return DbValue(field.as_double());
        case field_kind::string:
            return DbValue(std::string(field.as_string()));
        case field_kind::date:
        case field_kind::datetime:
        case field_kind::time:
            return DbValue(std::string(field.as_string()));
        default:
            return DbValue(std::string(field.as_string()));
    }
}

} // namespace gb
