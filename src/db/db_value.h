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
    STRING,
    BLOB,
    DATE,
    TIMESTAMP,
};

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
