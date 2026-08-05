#pragma once
#include <hiredis/hiredis.h>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <memory>

/// Redis 错误（替代 boost::system::error_code）。
/// code == 0 表示成功；非 0 表示失败。
struct RedisError
{
    int         code{0};
    std::string message;

    bool ok() const { return code == 0; }

    /// 对齐 boost::system::error_code 语义：true 表示出错。
    explicit operator bool() const { return code != 0; }
};

/// Redis 返回值（值树）。
///
/// 由 hiredis redisReply 同步拷贝而来，回调返回后即可安全持有。
/// 约定：
///   - is_nil()   表示 RESP NIL（key 不存在、LPop 空列表等）
///   - is_error() 表示 RESP ERROR 回复（命令本身报错）
///   - 传输层/连接错误通过回调的 RedisError 参数传递，不在此树内
class RedisValue
{
public:
    enum class Type : uint8_t
    {
        Nil = 0,
        Bool,
        Int,
        Double,
        Str,
        Array,
        Map,
        Error,
    };

    Type type{Type::Nil};

    bool                                            bool_val{false};
    int64_t                                         int_val{0};
    double                                          dbl_val{0.0};
    std::string                                     str_val;
    std::shared_ptr<std::vector<RedisValue>>              array_val;
    std::shared_ptr<std::vector<std::pair<std::string, RedisValue>>> map_val;
    std::string                                     error_msg;   ///< type == Error 时的错误信息

    // ── 工厂 ──
    static RedisValue MakeNil() { return {}; }
    static RedisValue MakeBool(bool v)
    {
        RedisValue r;
        r.type     = Type::Bool;
        r.bool_val = v;
        return r;
    }
    static RedisValue MakeInt(int64_t v)
    {
        RedisValue r;
        r.type    = Type::Int;
        r.int_val = v;
        return r;
    }
    static RedisValue MakeDouble(double v)
    {
        RedisValue r;
        r.type    = Type::Double;
        r.dbl_val = v;
        return r;
    }
    static RedisValue MakeStr(std::string v)
    {
        RedisValue r;
        r.type    = Type::Str;
        r.str_val = std::move(v);
        return r;
    }
    static RedisValue MakeArray(std::shared_ptr<std::vector<RedisValue>> v)
    {
        RedisValue r;
        r.type      = Type::Array;
        r.array_val = v;
        return r;
    }
    static RedisValue MakeMap(std::shared_ptr<std::vector<std::pair<std::string, RedisValue>>> v)
    {
        RedisValue r;
        r.type    = Type::Map;
        r.map_val = v;
        return r;
    }
    static RedisValue MakeError(std::string msg)
    {
        RedisValue r;
        r.type      = Type::Error;
        r.error_msg = std::move(msg);
        return r;
    }

    // ── 查询 ──
    bool is_nil() const { return type == Type::Nil; }
    bool is_error() const { return type == Type::Error; }
    bool is_bool() const { return type == Type::Bool; }
    bool is_int() const { return type == Type::Int; }
    bool is_double() const { return type == Type::Double; }
    bool is_str() const { return type == Type::Str; }
    bool is_array() const { return type == Type::Array; }
    bool is_map() const { return type == Type::Map; }

    /// 有值（非 nil 且非 error）。
    bool has_value() const { return type != Type::Nil && type != Type::Error; }

    /// 若本值为 RESP ERROR，返回对应 RedisError（否则返回成功 RedisError）。
    RedisError err() const
    {
        if (is_error())
            return RedisError{-1, error_msg};
        return RedisError{};
    }

    // ── 取值（类型不匹配时 LOG_ERROR 并返回默认值）──
    std::string as_str() const;
    int64_t     as_int() const;
    double      as_double() const;
    bool        as_bool() const;
    std::vector<std::string>                    as_str_array() const;
    std::vector<std::pair<std::string, double>> as_pairs() const;

    /// 从 hiredis 回复同步拷贝（reply 必须非空）。
    static RedisValue FromReply(const redisReply* reply);
};
