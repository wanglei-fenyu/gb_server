#include "pg_connection.h"
#include "log/log.h"
#include "async_simple/coro/FutureAwaiter.h"
#include <libpq-fe.h>
#include <boost/asio/post.hpp>
#include <cstring>
#include <sstream>

namespace gb
{

PgConnection::PgConnection(boost::asio::io_context& io_ctx)
    : io_ctx_(io_ctx)
{
}

PgConnection::~PgConnection()
{
    CloseSync();
}

// =========================================================================
// DbConnection 接口实现
// =========================================================================

bool PgConnection::ConnectSync(const DbConfig& cfg)
{
    config_ = cfg;
    // 关闭旧连接
    if (conn_) {
        PQfinish(conn_);
        conn_ = nullptr;
    }

    auto conn_str = BuildConnString(cfg);
    conn_ = PQconnectdb(conn_str.c_str());
    if (!conn_ || PQstatus(conn_) != CONNECTION_OK) {
        LOG_ERROR("[PgConnection] connect failed: {}",
                  conn_ ? PQerrorMessage(conn_) : "PQconnectdb returned null");
        connected_ = false;
        return false;
    }

    PQsetnonblocking(conn_, 1);
    connected_ = true;
    LOG_INFO("[PgConnection] connected to {}:{}/{}",
             config_.host, config_.port, config_.database);
    return true;
}

async_simple::coro::Lazy<bool> PgConnection::Connect(const DbConfig& cfg)
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

async_simple::coro::Lazy<void> PgConnection::Close()
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

bool PgConnection::IsConnected() const
{
    if (!conn_ || !connected_)
        return false;
    return PQstatus(conn_) == CONNECTION_OK;
}

async_simple::coro::Lazy<bool> PgConnection::Reset()
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

// =========================================================================
// 查询
// =========================================================================

async_simple::coro::Lazy<DbResult> PgConnection::Query(std::string_view sql)
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

async_simple::coro::Lazy<DbResult> PgConnection::Query(
    std::string_view sql, const std::vector<DbValue>& params)
{
    async_simple::Promise<DbResult> promise;
    auto future = promise.getFuture();

    auto self = shared_from_this();
    std::string sql_str(sql);
    boost::asio::post(io_ctx_, [this, self, sql = std::move(sql_str),
                                params, promise = std::move(promise)]() mutable {
        promise.setValue(SyncExecParams(sql.c_str(), params));
    });

    co_return co_await std::move(future);
}

async_simple::coro::Lazy<uint64_t> PgConnection::Execute(std::string_view sql)
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

async_simple::coro::Lazy<void> PgConnection::Begin()
{
    async_simple::Promise<void> promise;
    auto future = promise.getFuture();
    auto self = shared_from_this();

    boost::asio::post(io_ctx_, [this, self, promise = std::move(promise)]() mutable {
        SyncCommand("BEGIN");
        promise.setValue();
    });

    co_await std::move(future);
}

async_simple::coro::Lazy<void> PgConnection::Commit()
{
    async_simple::Promise<void> promise;
    auto future = promise.getFuture();
    auto self = shared_from_this();

    boost::asio::post(io_ctx_, [this, self, promise = std::move(promise)]() mutable {
        SyncCommand("COMMIT");
        promise.setValue();
    });

    co_await std::move(future);
}

async_simple::coro::Lazy<void> PgConnection::Rollback()
{
    async_simple::Promise<void> promise;
    auto future = promise.getFuture();
    auto self = shared_from_this();

    boost::asio::post(io_ctx_, [this, self, promise = std::move(promise)]() mutable {
        SyncCommand("ROLLBACK");
        promise.setValue();
    });

    co_await std::move(future);
}

// =========================================================================
// 内部 — 同步 libpq 操作（在 IO 线程上执行）
// =========================================================================

void PgConnection::CloseSync()
{
    connected_ = false;
    if (conn_) {
        PQfinish(conn_);
        conn_ = nullptr;
    }
}

std::string PgConnection::BuildConnString(const DbConfig& cfg)
{
    std::ostringstream ss;
    ss << "host=" << cfg.host;
    ss << " port=" << (cfg.port > 0 ? cfg.port : DefaultPort(DbType::POSTGRESQL));
    ss << " user=" << cfg.user;
    ss << " password=" << cfg.password;
    ss << " dbname=" << cfg.database;
    ss << " connect_timeout=" << cfg.connect_timeout_sec;
    if (cfg.use_ssl)
        ss << " sslmode=require";
    else
        ss << " sslmode=disable";
    return ss.str();
}

DbResult PgConnection::SyncQuery(const char* sql)
{
    if (!conn_ || PQstatus(conn_) != CONNECTION_OK) {
        return DbResult::Error("Connection not available");
    }

    // 使用同步 libpq（在 IO 线程上阻塞是安全的）
    PGresult* res = PQexec(conn_, sql);
    if (!res) {
        return DbResult::Error(PQerrorMessage(conn_));
    }

    DbResult result = ParseResult(res);
    PQclear(res);
    return result;
}

DbResult PgConnection::SyncExecParams(const char* sql, const std::vector<DbValue>& params)
{
    if (!conn_ || PQstatus(conn_) != CONNECTION_OK) {
        return DbResult::Error("Connection not available");
    }

    // 将 DbValue 转换为 libpq 参数
    std::vector<const char*> values;
    std::vector<int>         lengths;
    std::vector<int>         formats;  // 0=text, 1=binary
    std::vector<std::string> temp_strs;

    values.reserve(params.size());
    lengths.reserve(params.size());
    formats.reserve(params.size());
    temp_strs.reserve(params.size());

    for (const auto& p : params) {
        formats.push_back(0); // all text format
        if (p.is_null()) {
            values.push_back(nullptr);
            lengths.push_back(0);
        } else {
            switch (p.type()) {
                case DbFieldType::BOOL:
                    temp_strs.push_back(p.as_bool() ? "t" : "f");
                    break;
                case DbFieldType::INT8:
                    temp_strs.push_back(std::to_string(p.as_int8()));
                    break;
                case DbFieldType::INT16:
                    temp_strs.push_back(std::to_string(p.as_int16()));
                    break;
                case DbFieldType::INT32:
                    temp_strs.push_back(std::to_string(p.as_int32()));
                    break;
                case DbFieldType::INT64:
                    temp_strs.push_back(std::to_string(p.as_int64()));
                    break;
                case DbFieldType::UINT64:
                    temp_strs.push_back(std::to_string(p.as_uint64()));
                    break;
                case DbFieldType::FLOAT:
                    temp_strs.push_back(std::to_string(p.as_float()));
                    break;
                case DbFieldType::DOUBLE:
                    temp_strs.push_back(std::to_string(p.as_double()));
                    break;
                case DbFieldType::STRING: {
                    auto sv = p.as_string();
                    temp_strs.emplace_back(sv);
                    break;
                }
                default:
                    temp_strs.push_back("");
                    break;
            }
            values.push_back(temp_strs.back().c_str());
            lengths.push_back(static_cast<int>(temp_strs.back().size()));
        }
    }

    PGresult* res = PQexecParams(conn_, sql,
        static_cast<int>(params.size()), nullptr,
        values.data(), lengths.data(), formats.data(), 0);
    if (!res) {
        return DbResult::Error(PQerrorMessage(conn_));
    }

    DbResult result = ParseResult(res);
    PQclear(res);
    return result;
}

bool PgConnection::SyncCommand(const char* sql)
{
    if (!conn_ || PQstatus(conn_) != CONNECTION_OK) {
        return false;
    }
    PGresult* res = PQexec(conn_, sql);
    if (!res) {
        LOG_ERROR("[PgConnection] command failed: {} — {}", sql,
                  PQerrorMessage(conn_));
        return false;
    }
    auto status = PQresultStatus(res);
    PQclear(res);
    return status == PGRES_COMMAND_OK;
}

// =========================================================================
// 结果解析
// =========================================================================

DbResult PgConnection::ParseResult(PGresult* res)
{
    auto status = PQresultStatus(res);

    if (status == PGRES_TUPLES_OK) {
        // SELECT / RETURNING — 行结果集
        int n_rows = PQntuples(res);
        int n_cols = PQnfields(res);
        std::vector<DbRow> rows;
        rows.reserve(static_cast<size_t>(n_rows));

        for (int r = 0; r < n_rows; ++r) {
            std::vector<DbField> fields;
            fields.reserve(static_cast<size_t>(n_cols));
            for (int c = 0; c < n_cols; ++c) {
                const char* name  = PQfname(res, c);
                Oid         oid   = PQftype(res, c);
                if (PQgetisnull(res, r, c)) {
                    fields.emplace_back(name, DbValue(nullptr));
                } else {
                    fields.emplace_back(name, ToDbValue(PQgetvalue(res, r, c), oid));
                }
            }
            rows.emplace_back(std::move(fields));
        }
        return DbResult::Rows(std::move(rows), static_cast<uint32_t>(n_cols));
    }

    if (status == PGRES_COMMAND_OK) {
        // INSERT / UPDATE / DELETE
        const char* affected_str = PQcmdTuples(res);
        uint64_t affected = 0;
        if (affected_str && affected_str[0] != '\0')
            affected = std::stoull(affected_str);

        // 从 OID 值中获取 insert_id（仅当单行 INSERT 时有效）
        Oid oid = PQoidValue(res);
        return DbResult::AffectedRows(affected, oid != InvalidOid ? oid : 0);
    }

    // 错误
    return DbResult::Error(PQresultErrorMessage(res), static_cast<int>(status));
}

DbValue PgConnection::ToDbValue(const char* value, Oid type_oid)
{
    if (!value)
        return DbValue(nullptr);

    // libpq 返回文本格式，按 OID 类型转换
    // https://www.postgresql.org/docs/current/catalog-pg-type.html
    switch (type_oid) {
        case 16:   // bool
            return DbValue(value[0] == 't');
        case 20:   // int8  (int64)
        case 21:   // int2  (int16)
        case 23:   // int4  (int32)
            return DbValue(static_cast<int64_t>(std::atoll(value)));
        case 700:  // float4
            return DbValue(static_cast<float>(std::atof(value)));
        case 701:  // float8
            return DbValue(std::atof(value));
        case 1700: // numeric — 作为字符串保留
            [[fallthrough]];
        default:
            return DbValue(std::string(value));
    }
}

} // namespace gb
