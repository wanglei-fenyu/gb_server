#pragma once
#include "db_value.h"
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace gb
{

// ── 错误信息 ---------------------------------------------------------------

/// 数据库操作错误。
struct DbError
{
    int         code = 0;
    std::string message;
};

// ── 字段 -------------------------------------------------------------------

/// 结果集中一个字段的值。
class DbField
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

// ── 行 ---------------------------------------------------------------------

/// 结果集中的一行。
class DbRow
{
public:
    explicit DbRow(std::vector<DbField> fields) : fields_(std::move(fields)) {}

    size_t              field_count()                 const noexcept { return fields_.size(); }
    const DbField&      operator[](size_t idx)        const { return fields_.at(idx); }
    const DbField&      operator[](std::string_view name) const;

private:
    std::vector<DbField> fields_;
};

// ── 结果集 -----------------------------------------------------------------

/// 查询结果抽象。
///
/// 支持两种模式：
///   1. 行结果集（SELECT）→ has_next() / next() 迭代
///   2. 影响行数（INSERT/UPDATE/DELETE）→ affected_rows() / insert_id()
class DbResult
{
public:
    DbResult() = default;

    /// 创建行结果集。
    static DbResult Rows(std::vector<DbRow> rows, uint32_t col_count)
    {
        DbResult r;
        r.rows_       = std::move(rows);
        r.col_count_  = col_count;
        r.is_ok_      = true;
        return r;
    }

    /// 创建 DML 结果（INSERT/UPDATE/DELETE）。
    static DbResult AffectedRows(uint64_t affected, uint64_t insert_id = 0)
    {
        DbResult r;
        r.affected_   = affected;
        r.insert_id_  = insert_id;
        r.is_ok_      = true;
        return r;
    }

    /// 创建错误结果。
    static DbResult Error(std::string msg, int code = -1)
    {
        DbResult r;
        r.is_ok_     = false;
        r.error_     = DbError{code, std::move(msg)};
        return r;
    }

    // ---- 访问器 -----------------------------------------------------------

    bool     is_ok()          const noexcept { return is_ok_; }
    bool     is_error()       const noexcept { return !is_ok_; }
    const DbError& error()    const noexcept { return error_; }

    /// 影响行数（DML）。
    uint64_t affected_rows()  const noexcept { return affected_; }
    /// 最后插入 ID（INSERT）。
    uint64_t insert_id()      const noexcept { return insert_id_; }

    size_t   rows_count()     const noexcept { return rows_.size(); }
    uint32_t column_count()   const noexcept { return col_count_; }

    /// 是否有下一行。
    bool     has_next()       const noexcept { return row_cursor_ < rows_.size(); }
    /// 获取下一行并推进游标。
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

// ---- 内联实现 -------------------------------------------------------------

inline const DbField& DbRow::operator[](std::string_view name) const
{
    for (auto& f : fields_)
        if (f.name() == name)
            return f;
    static DbField s_null;
    return s_null;
}

} // namespace gb
