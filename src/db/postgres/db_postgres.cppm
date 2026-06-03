module;

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <queue>
#include <atomic>
#include <mutex>
#include <utility>
#include <libpq-fe.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>
#include <concurrentqueue.h>
#include <async_simple/coro/Lazy.h>
#include <async_simple/Future.h>
#include <async_simple/Promise.h>
#include "script/script.h"

export module db.postgres;

namespace gb {

// ══════════════════════════════════════════════════════════════════════════
// DbType
// ══════════════════════════════════════════════════════════════════════════

export enum class DbType : uint8_t
{
    POSTGRESQL
};

// ══════════════════════════════════════════════════════════════════════════
// DbConfig
// ══════════════════════════════════════════════════════════════════════════

export struct DbConfig
{
    std::string host     = "127.0.0.1";
    uint16_t    port     = 0;
    std::string user;
    std::string password;
    std::string database;
    bool        use_ssl  = false;

    int connect_timeout_sec = 5;
    int query_timeout_sec   = 10;

    size_t min_pool_size = 4;
    size_t max_pool_size = 32;

    static uint16_t DefaultPort(DbType /*type*/) noexcept
    {
        return 5432;
    }
};

// ══════════════════════════════════════════════════════════════════════════
// DbFieldType + DbFieldTypeName
// ══════════════════════════════════════════════════════════════════════════

export enum class DbFieldType : uint8_t
{
    NULL_TYPE,
    BOOL,
    INT8, INT16, INT32, INT64,
    UINT64,
    FLOAT,
    DOUBLE,
    NUMERIC,
    STRING,
    BLOB,
    DATE,
    TIME,
    TIMESTAMP,
    TIMESTAMPTZ,
    INTERVAL,
    UUID,
    JSON,
    INET,
    MACADDR,
    GEOMETRY,
    TSVECTOR,
    RANGE,
    PG_LSN,
    XML,
    ARRAY,
    MONEY,
    BIT,
};

export inline const char* DbFieldTypeName(DbFieldType t) noexcept
{
    switch (t) {
        case DbFieldType::NULL_TYPE:   return "NULL";
        case DbFieldType::BOOL:        return "BOOL";
        case DbFieldType::INT8:        return "INT8";
        case DbFieldType::INT16:       return "INT16";
        case DbFieldType::INT32:       return "INT32";
        case DbFieldType::INT64:       return "INT64";
        case DbFieldType::UINT64:      return "UINT64";
        case DbFieldType::FLOAT:       return "FLOAT";
        case DbFieldType::DOUBLE:      return "DOUBLE";
        case DbFieldType::NUMERIC:     return "NUMERIC";
        case DbFieldType::STRING:      return "STRING";
        case DbFieldType::BLOB:        return "BLOB";
        case DbFieldType::DATE:        return "DATE";
        case DbFieldType::TIME:        return "TIME";
        case DbFieldType::TIMESTAMP:   return "TIMESTAMP";
        case DbFieldType::TIMESTAMPTZ: return "TIMESTAMPTZ";
        case DbFieldType::INTERVAL:    return "INTERVAL";
        case DbFieldType::UUID:        return "UUID";
        case DbFieldType::JSON:        return "JSON";
        case DbFieldType::INET:        return "INET";
        case DbFieldType::MACADDR:     return "MACADDR";
        case DbFieldType::GEOMETRY:    return "GEOMETRY";
        case DbFieldType::TSVECTOR:    return "TSVECTOR";
        case DbFieldType::RANGE:       return "RANGE";
        case DbFieldType::PG_LSN:      return "PG_LSN";
        case DbFieldType::XML:         return "XML";
        case DbFieldType::ARRAY:       return "ARRAY";
        case DbFieldType::MONEY:       return "MONEY";
        case DbFieldType::BIT:         return "BIT";
        default:                       return "UNKNOWN";
    }
}

// ══════════════════════════════════════════════════════════════════════════
// DbValue
// ══════════════════════════════════════════════════════════════════════════

export class DbValue
{
public:
    DbValue() noexcept : type_(DbFieldType::NULL_TYPE) {}

    explicit DbValue(std::nullptr_t)  noexcept : type_(DbFieldType::NULL_TYPE) {}
    explicit DbValue(bool v)          noexcept : type_(DbFieldType::BOOL),   bool_val_(v) {}
    explicit DbValue(int8_t v)        noexcept : type_(DbFieldType::INT8),   int8_val_(v) {}
    explicit DbValue(int16_t v)       noexcept : type_(DbFieldType::INT16),  int16_val_(v) {}
    explicit DbValue(int32_t v)       noexcept : type_(DbFieldType::INT32),  int32_val_(v) {}
    explicit DbValue(int64_t v)       noexcept : type_(DbFieldType::INT64),  int64_val_(v) {}
    explicit DbValue(uint64_t v)      noexcept : type_(DbFieldType::UINT64), uint64_val_(v) {}
    explicit DbValue(float v)         noexcept : type_(DbFieldType::FLOAT),  float_val_(v) {}
    explicit DbValue(double v)        noexcept : type_(DbFieldType::DOUBLE), double_val_(v) {}
    explicit DbValue(std::string v)   noexcept : type_(DbFieldType::STRING), str_val_(std::move(v)) {}
    explicit DbValue(const char* v)   noexcept : type_(DbFieldType::STRING), str_val_(v) {}
    explicit DbValue(std::string_view v) : type_(DbFieldType::STRING), str_val_(v) {}

    explicit DbValue(std::string v, DbFieldType type) noexcept
      : type_(type), str_val_(std::move(v)) {}

    DbFieldType type()  const noexcept { return type_; }
    bool        is_null() const noexcept { return type_ == DbFieldType::NULL_TYPE; }

    bool        as_bool()   const;
    int8_t      as_int8()   const;
    int16_t     as_int16()  const;
    int32_t     as_int32()  const;
    int64_t     as_int64()  const;
    uint64_t    as_uint64() const;
    float       as_float()  const;
    double      as_double() const;
    std::string_view as_string() const;

private:
    DbFieldType type_;

    union {
        bool     bool_val_;
        int8_t   int8_val_;
        int16_t  int16_val_;
        int32_t  int32_val_;
        int64_t  int64_val_;
        uint64_t uint64_val_;
        float    float_val_;
        double   double_val_;
    };
    std::string str_val_;
};

// ── inline 访问器 ──

inline bool DbValue::as_bool() const { return bool_val_; }
inline int8_t DbValue::as_int8() const { return int8_val_; }
inline int16_t DbValue::as_int16() const { return int16_val_; }
inline int32_t DbValue::as_int32() const { return int32_val_; }
inline int64_t DbValue::as_int64() const { return int64_val_; }
inline uint64_t DbValue::as_uint64() const { return uint64_val_; }
inline float DbValue::as_float() const { return float_val_; }
inline double DbValue::as_double() const { return double_val_; }
inline std::string_view DbValue::as_string() const { return str_val_; }

// ══════════════════════════════════════════════════════════════════════════
// DbError / DbField / DbRow / DbResult
// ══════════════════════════════════════════════════════════════════════════

export struct DbError
{
    int         code = 0;
    std::string message;
};

export class DbField
{
public:
    DbField() = default;
    DbField(std::string name, DbValue value)
        : name_(std::move(name)), value_(std::move(value)) {}

    const std::string& name()  const noexcept { return name_; }
    const DbValue&     value() const noexcept { return value_; }
    DbFieldType        type()  const noexcept { return value_.type(); }
    bool               is_null() const noexcept { return value_.is_null(); }

    bool        as_bool()   const { return value_.as_bool(); }
    int8_t      as_int8()   const { return value_.as_int8(); }
    int16_t     as_int16()  const { return value_.as_int16(); }
    int32_t     as_int32()  const { return value_.as_int32(); }
    int64_t     as_int64()  const { return value_.as_int64(); }
    uint64_t    as_uint64() const { return value_.as_uint64(); }
    float       as_float()  const { return value_.as_float(); }
    double      as_double() const { return value_.as_double(); }
    std::string_view as_string() const { return value_.as_string(); }

private:
    std::string name_;
    DbValue     value_;
};

export class DbRow
{
public:
    explicit DbRow(std::vector<DbField> fields) : fields_(std::move(fields)) {}

    size_t              field_count()                 const noexcept { return fields_.size(); }
    const DbField&      operator[](size_t idx)        const { return fields_.at(idx); }
    const DbField&      operator[](std::string_view name) const;

private:
    std::vector<DbField> fields_;
};

export class DbResult
{
public:
    DbResult() = default;

    static DbResult Rows(std::vector<DbRow> rows, uint32_t col_count)
    {
        DbResult r;
        r.rows_       = std::move(rows);
        r.col_count_  = col_count;
        r.is_ok_      = true;
        return r;
    }

    static DbResult AffectedRows(uint64_t affected, uint64_t insert_id = 0)
    {
        DbResult r;
        r.affected_   = affected;
        r.insert_id_  = insert_id;
        r.is_ok_      = true;
        return r;
    }

    static DbResult Error(std::string msg, int code = -1)
    {
        DbResult r;
        r.is_ok_     = false;
        r.error_     = DbError{code, std::move(msg)};
        return r;
    }

    bool     is_ok()          const noexcept { return is_ok_; }
    bool     is_error()       const noexcept { return !is_ok_; }
    const DbError& error()    const noexcept { return error_; }

    uint64_t affected_rows()  const noexcept { return affected_; }
    uint64_t insert_id()      const noexcept { return insert_id_; }

    size_t   rows_count()     const noexcept { return rows_.size(); }
    uint32_t column_count()   const noexcept { return col_count_; }

    bool     has_next()       const noexcept { return row_cursor_ < rows_.size(); }
    DbRow    next()
    {
        if (row_cursor_ < rows_.size())
            return rows_[row_cursor_++];
        return DbRow({});
    }

private:
    bool         is_ok_     = false;
    DbError      error_;

    uint64_t     affected_  = 0;
    uint64_t     insert_id_ = 0;

    std::vector<DbRow> rows_;
    size_t       row_cursor_ = 0;
    uint32_t     col_count_  = 0;
};

// ── inline ──

inline const DbField& DbRow::operator[](std::string_view name) const
{
    for (auto& f : fields_)
        if (f.name() == name)
            return f;
    static DbField s_null;
    return s_null;
}

// ══════════════════════════════════════════════════════════════════════════
// DbConnection (abstract)
// ══════════════════════════════════════════════════════════════════════════

export class PgConnection; // forward decl for friend

export class DbConnection : public std::enable_shared_from_this<DbConnection>
{
public:
    virtual ~DbConnection() = default;

    virtual DbType type() const noexcept = 0;

    virtual async_simple::coro::Lazy<bool> Connect(const DbConfig& cfg) = 0;
    virtual async_simple::coro::Lazy<void> Close() = 0;
    virtual bool IsConnected() const = 0;
    virtual async_simple::coro::Lazy<bool> Reset() = 0;

    virtual async_simple::coro::Lazy<DbResult> Query(std::string_view sql) = 0;
    virtual async_simple::coro::Lazy<DbResult> Query(
        std::string_view sql, const std::vector<DbValue>& params) = 0;
    virtual async_simple::coro::Lazy<uint64_t> Execute(std::string_view sql) = 0;

    virtual async_simple::coro::Lazy<void> Begin()   = 0;
    virtual async_simple::coro::Lazy<void> Commit()  = 0;
    virtual async_simple::coro::Lazy<void> Rollback() = 0;

    template <typename T, typename... Args>
    static std::shared_ptr<T> Create(Args&&... args)
    {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }
};

export using DbConnectionPtr = std::shared_ptr<DbConnection>;

// ══════════════════════════════════════════════════════════════════════════
// PgConnection
// ══════════════════════════════════════════════════════════════════════════

export class PgConnection final : public DbConnection
{
public:
    explicit PgConnection(boost::asio::io_context& io_ctx);
    ~PgConnection() override;

    PgConnection(const PgConnection&) = delete;
    PgConnection& operator=(const PgConnection&) = delete;

    // ── DbConnection 接口 ──
    DbType type() const noexcept override { return DbType::POSTGRESQL; }

    async_simple::coro::Lazy<bool>          Connect(const DbConfig& cfg) override;
    async_simple::coro::Lazy<void>          Close() override;
    bool                                    IsConnected() const override;
    async_simple::coro::Lazy<bool>          Reset() override;

    // ── 同步接口 ──
    void CloseSync();
    bool ConnectSync(const DbConfig& cfg);

    async_simple::coro::Lazy<DbResult>      Query(std::string_view sql) override;
    async_simple::coro::Lazy<DbResult>      Query(
        std::string_view sql, const std::vector<DbValue>& params) override;
    async_simple::coro::Lazy<uint64_t>      Execute(std::string_view sql) override;

    async_simple::coro::Lazy<void>          Begin() override;
    async_simple::coro::Lazy<void>          Commit() override;
    async_simple::coro::Lazy<void>          Rollback() override;

    // ── 异步回调 API ──
    void AsyncConnect(const DbConfig& cfg, std::function<void(bool)> callback);
    void AsyncClose(std::function<void()> callback);
    void AsyncQuery(std::string_view sql, std::function<void(DbResult)> callback);
    void AsyncQuery(std::string_view sql, const std::vector<DbValue>& params,
                    std::function<void(DbResult)> callback);
    void AsyncExecute(std::string_view sql, std::function<void(uint64_t)> callback);
    void AsyncBegin(std::function<void(bool)> callback);
    void AsyncCommit(std::function<void(bool)> callback);
    void AsyncRollback(std::function<void(bool)> callback);

private:
    struct AsyncOp;

    void StartSend(std::function<bool(PGconn*)> send_fn,
                   std::function<void(DbResult)> callback);
    void ContinueOp(std::shared_ptr<AsyncOp> op);
    std::shared_ptr<boost::asio::ip::tcp::socket> WrapPgSocket();
    void StartNext();

    static DbResult ParseResult(PGresult* res);
    static DbValue ToDbValue(const char* value, Oid type_oid);
    std::string BuildConnString(const DbConfig& cfg);

private:
    boost::asio::io_context&           io_ctx_;
    PGconn*                            conn_        = nullptr;
    DbConfig                           config_;
    bool                               connected_   = false;

    std::queue<std::shared_ptr<AsyncOp>> pending_ops_;
    bool                                 op_active_ = false;
};

// ══════════════════════════════════════════════════════════════════════════
// DbConnectionPool
// ══════════════════════════════════════════════════════════════════════════

export class DbConnectionPool : public std::enable_shared_from_this<DbConnectionPool>
{
public:
    static std::shared_ptr<DbConnectionPool> Create(
        boost::asio::io_context& io_ctx, const DbConfig& cfg);

    ~DbConnectionPool();

    DbConnectionPool(const DbConnectionPool&) = delete;
    DbConnectionPool& operator=(const DbConnectionPool&) = delete;

    // ── 生命周期 ──
    async_simple::coro::Lazy<bool> Start();
    async_simple::coro::Lazy<void> Stop();

    // ── 连接获取/归还 ──
    async_simple::coro::Lazy<DbConnectionPtr> Acquire();
    void Release(DbConnectionPtr conn);

    // ── 状态 ──
    size_t IdleCount()      const noexcept { return idle_count_.load(); }
    size_t ActiveCount()    const noexcept { return active_count_.load(); }
    size_t TotalCount()     const noexcept { return total_connections_.load(); }
    bool   IsRunning()      const noexcept { return running_.load(); }

private:
    DbConnectionPool(boost::asio::io_context& io_ctx, const DbConfig& cfg);

    void DoAcquire(async_simple::Promise<DbConnectionPtr> promise);
    void DoRelease(DbConnectionPtr conn);
    DbConnectionPtr CreateConnectionSync();
    void StartHeartbeat();
    void OnHeartbeat(const boost::system::error_code& ec);

private:
    boost::asio::io_context&     io_ctx_;

    DbConfig                     config_;
    DbType                       db_type_;

    moodycamel::ConcurrentQueue<DbConnectionPtr> idle_;
    std::atomic<size_t>          total_connections_{0};
    std::atomic<size_t>          idle_count_{0};
    std::atomic<size_t>          active_count_{0};
    std::atomic<bool>            running_{false};

    struct Waiter {
        async_simple::Promise<DbConnectionPtr> promise;
    };
    moodycamel::ConcurrentQueue<Waiter> waiters_;

    boost::asio::steady_timer    heartbeat_timer_;
    static constexpr int         HEARTBEAT_INTERVAL_SEC = 30;
};

export using DbConnectionPoolPtr = std::shared_ptr<DbConnectionPool>;

} // namespace gb

// ══════════════════════════════════════════════════════════════════════════
// 注册函数
// ══════════════════════════════════════════════════════════════════════════

export void register_postgresql(std::shared_ptr<Script>& scriptPtr);
