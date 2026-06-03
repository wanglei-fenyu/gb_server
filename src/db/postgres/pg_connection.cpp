#include "pg_connection.h"
#include "log/log.h"
#include "async_simple/coro/FutureAwaiter.h"
#include <libpq-fe.h>
#include <boost/asio/post.hpp>
#include <boost/system/error_code.hpp>
#include <cstring>
#include <sstream>

namespace gb
{

// =========================================================================
// AsyncOp 内部结构
// =========================================================================

struct PgConnection::AsyncOp {
    using Callback = std::function<void(DbResult)>;
    using SendFn   = std::function<bool(PGconn*)>;

    SendFn     send_fn;    // 仅入队时使用
    Callback   callback;
    bool       flushing = true;   // true = 仍在 flush 阶段
};

// 为方便函数定义
using AsyncOpSendFn   = std::function<bool(PGconn*)>;
using AsyncOpCallback = std::function<void(DbResult)>;

// =========================================================================
// 构造 / 析构
// =========================================================================

PgConnection::PgConnection(boost::asio::io_context& io_ctx)
    : io_ctx_(io_ctx)
{
}

PgConnection::~PgConnection()
{
    CloseSync();
}

// =========================================================================
// 异步管道 — libpq 非阻塞模式 + Boost.Asio reactor
//
// 完整流程：
//   1. PQsendQuery / PQsendQueryParams  → 立即返回（非阻塞）
//   2. PQflush       → 若返回 1，等待 socket 可写后重试
//   3. PQconsumeInput → 读取服务端数据
//   4. PQisBusy      → 若返回 1，等待 socket 可读后重试
//   5. PQgetResult   → 收集结果（循环）
//   6. callback(result)
//   7. StartNext()   → 处理队列中下一个请求
// =========================================================================

void PgConnection::StartSend(AsyncOpSendFn send_fn, AsyncOpCallback callback)
{
    // 已有操作在进行 → 入队等待
    if (op_active_) {
        auto op          = std::make_shared<AsyncOp>();
        op->send_fn      = std::move(send_fn);
        op->callback     = std::move(callback);
        op->flushing     = true;
        pending_ops_.push(std::move(op));
        return;
    }

    op_active_ = true;

    if (!conn_ || PQstatus(conn_) != CONNECTION_OK) {
        callback(DbResult::Error("Connection not available"));
        StartNext();
        return;
    }

    auto op         = std::make_shared<AsyncOp>();
    op->callback    = std::move(callback);
    op->flushing    = true;

    if (!send_fn(conn_)) {
        op->callback(DbResult::Error(PQerrorMessage(conn_)));
        StartNext();
        return;
    }

    ContinueOp(std::move(op));
}

void PgConnection::ContinueOp(std::shared_ptr<AsyncOp> op)
{
    auto self = shared_from_this();

    // ── 阶段 1：Flush（确保所有数据已发送） ────────────────────────────
    if (op->flushing) {
        int status = PQflush(conn_);
        if (status == -1) {
            op->callback(DbResult::Error(PQerrorMessage(conn_)));
            StartNext();
            return;
        }
        if (status == 1) {
            // 数据未发完 → 等待 socket 可写
            auto sock = WrapPgSocket();
            if (!sock) {
                op->callback(DbResult::Error("PQsocket failed"));
                StartNext();
                return;
            }
            sock->async_wait(boost::asio::ip::tcp::socket::wait_write,
                [this, self, sock, op](const boost::system::error_code& ec) mutable {
                    sock->release();
                    if (ec) {
                        op->callback(DbResult::Error(
                            "socket write: " + ec.message()));
                        StartNext();
                        return;
                    }
                    ContinueOp(op);
                });
            return;
        }
        // Flush 完成 → 进入消费阶段
        op->flushing = false;
    }

    // ── 阶段 2：消费输入 ──────────────────────────────────────────────
    if (PQconsumeInput(conn_) == 0) {
        op->callback(DbResult::Error(PQerrorMessage(conn_)));
        StartNext();
        return;
    }

    if (PQisBusy(conn_)) {
        // 结果未就绪 → 等待 socket 可读
        auto sock = WrapPgSocket();
        if (!sock) {
            op->callback(DbResult::Error("PQsocket failed"));
            StartNext();
            return;
        }
        sock->async_wait(boost::asio::ip::tcp::socket::wait_read,
            [this, self, sock, op](const boost::system::error_code& ec) mutable {
                sock->release();
                if (ec) {
                    op->callback(DbResult::Error(
                        "socket read: " + ec.message()));
                    StartNext();
                    return;
                }
                ContinueOp(op);
            });
        return;
    }

    // ── 阶段 3：收集结果 ──────────────────────────────────────────────
    DbResult final_result = DbResult::Error("no result");
    while (PGresult* res = PQgetResult(conn_)) {
        auto parsed = ParseResult(res);
        if (parsed.is_ok()) {
            // 最后一个非错误结果胜出
            final_result = std::move(parsed);
        } else if (final_result.is_error()) {
            // 保留第一个错误
            final_result = std::move(parsed);
        }
        PQclear(res);
    }

    op->callback(std::move(final_result));
    StartNext();
}

std::shared_ptr<boost::asio::ip::tcp::socket> PgConnection::WrapPgSocket()
{
    auto sock    = std::make_shared<boost::asio::ip::tcp::socket>(io_ctx_);
    int pg_sock = PQsocket(conn_);
    if (pg_sock == -1)
        return nullptr;
    sock->assign(boost::asio::ip::tcp::v4(),
        static_cast<boost::asio::ip::tcp::socket::native_handle_type>(pg_sock));
    return sock;
}

void PgConnection::StartNext()
{
    op_active_ = false;
    if (!pending_ops_.empty()) {
        auto next = std::move(pending_ops_.front());
        pending_ops_.pop();
        auto send_fn  = std::move(next->send_fn);
        auto callback = std::move(next->callback);
        StartSend(std::move(send_fn), std::move(callback));
    }
}

// =========================================================================
// DbConnection 接口实现
// =========================================================================

bool PgConnection::ConnectSync(const DbConfig& cfg)
{
    config_ = cfg;
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
    auto cfg = config_;
    boost::asio::post(io_ctx_, [this, self, cfg,
                                promise = std::move(promise)]() mutable {
        CloseSync();
        promise.setValue(ConnectSync(cfg));
    });

    co_return co_await std::move(future);
}

// =========================================================================
// 查询 — 基于异步管道
// =========================================================================

async_simple::coro::Lazy<DbResult> PgConnection::Query(std::string_view sql)
{
    async_simple::Promise<DbResult> promise;
    auto future = promise.getFuture();

    auto self = shared_from_this();
    std::string sql_str(sql);
    boost::asio::post(io_ctx_, [this, self, sql = std::move(sql_str),
                                promise = std::move(promise)]() mutable {
        StartSend(
            [sql = std::move(sql)](PGconn* conn) -> bool {
                return PQsendQuery(conn, sql.c_str()) == 1;
            },
            [promise = std::move(promise)](DbResult result) mutable {
                promise.setValue(std::move(result));
            });
    });

    co_return co_await std::move(future);
}

async_simple::coro::Lazy<DbResult> PgConnection::Query(
    std::string_view sql, const std::vector<DbValue>& params)
{
    async_simple::Promise<DbResult> promise;
    auto future = promise.getFuture();

    auto self = shared_from_this();
    // 准备工作：转换参数（在调用线程上完成）
    auto sql_str = std::make_shared<std::string>(sql);
    auto params_copy = std::make_shared<std::vector<DbValue>>(params);

    boost::asio::post(io_ctx_, [this, self, sql_str, params_copy,
                                promise = std::move(promise)]() mutable {
        // 将 DbValue 转换为 libpq 参数
        std::vector<const char*> values;
        std::vector<int>         lengths;
        std::vector<int>         formats;
        std::vector<std::string> temp_strs;

        values.reserve(params_copy->size());
        lengths.reserve(params_copy->size());
        formats.reserve(params_copy->size());
        temp_strs.reserve(params_copy->size());

        for (const auto& p : *params_copy) {
            formats.push_back(0); // text format
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
                        temp_strs.emplace_back(p.as_string());
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

        // 注意：temp_strs 必须在 StartSend 期间保持存活
        auto temp_holder = std::make_shared<std::vector<std::string>>(std::move(temp_strs));

        StartSend(
            [sql = *sql_str, temp_holder,
             n_params = static_cast<int>(params_copy->size()),
             values, lengths, formats](PGconn* conn) -> bool {
                return PQsendQueryParams(conn, sql.c_str(),
                    n_params, nullptr, values.data(),
                    lengths.data(), formats.data(), 0) == 1;
            },
            [promise = std::move(promise)](DbResult result) mutable {
                promise.setValue(std::move(result));
            });
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
        StartSend(
            [sql = std::move(sql)](PGconn* conn) -> bool {
                return PQsendQuery(conn, sql.c_str()) == 1;
            },
            [promise = std::move(promise)](DbResult result) mutable {
                uint64_t affected = result.is_ok() ? result.affected_rows() : 0;
                promise.setValue(affected);
            });
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
        StartSend(
            [](PGconn* conn) -> bool {
                return PQsendQuery(conn, "BEGIN") == 1;
            },
            [promise = std::move(promise)](DbResult) mutable {
                promise.setValue();
            });
    });

    co_await std::move(future);
}

async_simple::coro::Lazy<void> PgConnection::Commit()
{
    async_simple::Promise<void> promise;
    auto future = promise.getFuture();
    auto self = shared_from_this();

    boost::asio::post(io_ctx_, [this, self, promise = std::move(promise)]() mutable {
        StartSend(
            [](PGconn* conn) -> bool {
                return PQsendQuery(conn, "COMMIT") == 1;
            },
            [promise = std::move(promise)](DbResult) mutable {
                promise.setValue();
            });
    });

    co_await std::move(future);
}

async_simple::coro::Lazy<void> PgConnection::Rollback()
{
    async_simple::Promise<void> promise;
    auto future = promise.getFuture();
    auto self = shared_from_this();

    boost::asio::post(io_ctx_, [this, self, promise = std::move(promise)]() mutable {
        StartSend(
            [](PGconn* conn) -> bool {
                return PQsendQuery(conn, "ROLLBACK") == 1;
            },
            [promise = std::move(promise)](DbResult) mutable {
                promise.setValue();
            });
    });

    co_await std::move(future);
}

// =========================================================================
// 异步回调 API — 非阻塞，IO 线程从不阻塞
// =========================================================================

void PgConnection::AsyncConnect(const DbConfig& cfg, std::function<void(bool)> callback)
{
    auto self = shared_from_this();
    boost::asio::post(io_ctx_, [this, self, cfg, cb = std::move(callback)]() mutable {
        cb(ConnectSync(cfg));
    });
}

void PgConnection::AsyncClose(std::function<void()> callback)
{
    auto self = shared_from_this();
    boost::asio::post(io_ctx_, [this, self, cb = std::move(callback)]() mutable {
        CloseSync();
        cb();
    });
}

void PgConnection::AsyncQuery(std::string_view sql, std::function<void(DbResult)> callback)
{
    auto self = shared_from_this();
    std::string sql_str(sql);
    boost::asio::post(io_ctx_, [this, self, sql = std::move(sql_str),
                                cb = std::move(callback)]() mutable {
        StartSend(
            [sql = std::move(sql)](PGconn* conn) -> bool {
                return PQsendQuery(conn, sql.c_str()) == 1;
            },
            [cb = std::move(cb)](DbResult result) mutable {
                cb(std::move(result));
            });
    });
}

void PgConnection::AsyncQuery(std::string_view sql, const std::vector<DbValue>& params,
                              std::function<void(DbResult)> callback)
{
    auto self = shared_from_this();
    auto sql_str  = std::make_shared<std::string>(sql);
    auto params_copy = std::make_shared<std::vector<DbValue>>(params);

    boost::asio::post(io_ctx_, [this, self, sql_str, params_copy,
                                cb = std::move(callback)]() mutable {
        // 转换参数
        std::vector<const char*> values;
        std::vector<int>         lengths;
        std::vector<int>         formats;
        std::vector<std::string> temp_strs;

        values.reserve(params_copy->size());
        lengths.reserve(params_copy->size());
        formats.reserve(params_copy->size());
        temp_strs.reserve(params_copy->size());

        for (const auto& p : *params_copy) {
            formats.push_back(0);
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
                    case DbFieldType::STRING:
                        temp_strs.emplace_back(p.as_string());
                        break;
                    default:
                        temp_strs.push_back("");
                        break;
                }
                values.push_back(temp_strs.back().c_str());
                lengths.push_back(static_cast<int>(temp_strs.back().size()));
            }
        }

        auto temp_holder = std::make_shared<std::vector<std::string>>(std::move(temp_strs));

        StartSend(
            [sql = *sql_str, temp_holder,
             n_params = static_cast<int>(params_copy->size()),
             values, lengths, formats](PGconn* conn) -> bool {
                return PQsendQueryParams(conn, sql.c_str(),
                    n_params, nullptr, values.data(),
                    lengths.data(), formats.data(), 0) == 1;
            },
            [cb = std::move(cb)](DbResult result) mutable {
                cb(std::move(result));
            });
    });
}

void PgConnection::AsyncExecute(std::string_view sql, std::function<void(uint64_t)> callback)
{
    auto self = shared_from_this();
    std::string sql_str(sql);
    boost::asio::post(io_ctx_, [this, self, sql = std::move(sql_str),
                                cb = std::move(callback)]() mutable {
        StartSend(
            [sql = std::move(sql)](PGconn* conn) -> bool {
                return PQsendQuery(conn, sql.c_str()) == 1;
            },
            [cb = std::move(cb)](DbResult result) mutable {
                cb(result.is_ok() ? result.affected_rows() : 0);
            });
    });
}

void PgConnection::AsyncBegin(std::function<void(bool)> callback)
{
    auto self = shared_from_this();
    boost::asio::post(io_ctx_, [this, self, cb = std::move(callback)]() mutable {
        StartSend(
            [](PGconn* conn) -> bool {
                return PQsendQuery(conn, "BEGIN") == 1;
            },
            [cb = std::move(cb)](DbResult result) mutable {
                cb(result.is_ok());
            });
    });
}

void PgConnection::AsyncCommit(std::function<void(bool)> callback)
{
    auto self = shared_from_this();
    boost::asio::post(io_ctx_, [this, self, cb = std::move(callback)]() mutable {
        StartSend(
            [](PGconn* conn) -> bool {
                return PQsendQuery(conn, "COMMIT") == 1;
            },
            [cb = std::move(cb)](DbResult result) mutable {
                cb(result.is_ok());
            });
    });
}

void PgConnection::AsyncRollback(std::function<void(bool)> callback)
{
    auto self = shared_from_this();
    boost::asio::post(io_ctx_, [this, self, cb = std::move(callback)]() mutable {
        StartSend(
            [](PGconn* conn) -> bool {
                return PQsendQuery(conn, "ROLLBACK") == 1;
            },
            [cb = std::move(cb)](DbResult result) mutable {
                cb(result.is_ok());
            });
    });
}

// =========================================================================
// 内部工具
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
    ss << " port=" << (cfg.port > 0 ? cfg.port : DbConfig::DefaultPort(DbType::POSTGRESQL));
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
