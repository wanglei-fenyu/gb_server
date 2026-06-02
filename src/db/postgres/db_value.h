#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <string_view>

namespace gb
{

/// 数据库字段类型枚举。
enum class DbFieldType : uint8_t
{
    NULL_TYPE,
    BOOL,
    INT8, INT16, INT32, INT64,
    UINT64,
    FLOAT,
    DOUBLE,
    NUMERIC,      // numeric/decimal — exact precision, stored as string
    STRING,       // text/varchar/char/unknown
    BLOB,         // bytea
    DATE,
    TIME,
    TIMESTAMP,    // timestamp without time zone
    TIMESTAMPTZ,  // timestamp with time zone
    INTERVAL,
    UUID,
    JSON,         // json / jsonb
    INET,         // inet / cidr
    MACADDR,
    GEOMETRY,     // point/lseg/box/path/polygon/circle/line
    TSVECTOR,     // tsvector / tsquery
    RANGE,        // int4range/int8range/numrange/tsrange/tstzrange/daterange
    PG_LSN,
    XML,
    ARRAY,        // PostgreSQL array (text representation)
    MONEY,
    BIT,          // bit / bit varying
};

/// 返回 DbFieldType 的人类可读名称（用于日志/调试）。
inline const char* DbFieldTypeName(DbFieldType t) noexcept
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

/// 可空字段值——用于参数化查询或结果读取。
class DbValue
{
public:
    DbValue() noexcept : type_(DbFieldType::NULL_TYPE) {}

    // ---- 构造（C++ 值 → DbValue） -----------------------------------------
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

    /// 构造一个带特定类型标签的字符串值（用于 DATE/TIME/UUID/JSON 等复杂 PG 类型）。
    /// 内部将值存储在 str_val_ 中，但 type() 返回指定的 type 而非 STRING。
    explicit DbValue(std::string v, DbFieldType type) noexcept
      : type_(type), str_val_(std::move(v)) {}

    // ---- 访问器 -----------------------------------------------------------
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

// ---- 内联实现 -------------------------------------------------------------

inline bool DbValue::as_bool() const { return bool_val_; }
inline int8_t DbValue::as_int8() const { return int8_val_; }
inline int16_t DbValue::as_int16() const { return int16_val_; }
inline int32_t DbValue::as_int32() const { return int32_val_; }
inline int64_t DbValue::as_int64() const { return int64_val_; }
inline uint64_t DbValue::as_uint64() const { return uint64_val_; }
inline float DbValue::as_float() const { return float_val_; }
inline double DbValue::as_double() const { return double_val_; }
inline std::string_view DbValue::as_string() const { return str_val_; }

} // namespace gb
