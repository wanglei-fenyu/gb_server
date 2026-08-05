#include "redis_value.h"
#include "log/log.h"
#include <cstdlib>

namespace
{

/// 字符串 → double（非法输入返回 0.0，避免异常逃逸到 hiredis 回调）
double SafeStod(const std::string& s)
{
    if (s.empty())
        return 0.0;
    char* end = nullptr;
    double v  = std::strtod(s.c_str(), &end);
    if (end == s.c_str())
    {
        LOG_ERROR("Redis parse double failed: \"{}\"", s);
        return 0.0;
    }
    return v;
}

/// 字符串 → int64（非法输入返回 0）
int64_t SafeStoll(const std::string& s)
{
    if (s.empty())
        return 0;
    char* end = nullptr;
    long long v = std::strtoll(s.c_str(), &end, 10);
    if (end == s.c_str())
    {
        LOG_ERROR("Redis parse int failed: \"{}\"", s);
        return 0;
    }
    return static_cast<int64_t>(v);
}

} // anonymous namespace

std::string RedisValue::as_str() const
{
    switch (type)
    {
    case Type::Str:
        return str_val;
    case Type::Int:
        return std::to_string(int_val);
    case Type::Double:
        return std::to_string(dbl_val);
    case Type::Bool:
        return bool_val ? "1" : "0";
    default:
        LOG_ERROR("RedisValue::as_str on type {}", static_cast<int>(type));
        return {};
    }
}

int64_t RedisValue::as_int() const
{
    switch (type)
    {
    case Type::Int:
        return int_val;
    case Type::Bool:
        return bool_val ? 1 : 0;
    case Type::Double:
        return static_cast<int64_t>(dbl_val);
    case Type::Str:
        return SafeStoll(str_val);
    default:
        LOG_ERROR("RedisValue::as_int on type {}", static_cast<int>(type));
        return 0;
    }
}

double RedisValue::as_double() const
{
    switch (type)
    {
    case Type::Double:
        return dbl_val;
    case Type::Int:
        return static_cast<double>(int_val);
    case Type::Str:
        return SafeStod(str_val);
    default:
        LOG_ERROR("RedisValue::as_double on type {}", static_cast<int>(type));
        return 0.0;
    }
}

bool RedisValue::as_bool() const
{
    switch (type)
    {
    case Type::Bool:
        return bool_val;
    case Type::Int:
        return int_val != 0;
    case Type::Str:
        return str_val == "1" || str_val == "true";
    default:
        LOG_ERROR("RedisValue::as_bool on type {}", static_cast<int>(type));
        return false;
    }
}

std::vector<std::string> RedisValue::as_str_array() const
{
    std::vector<std::string> out;
    if (type != Type::Array)
    {
        LOG_ERROR("RedisValue::as_str_array on type {}", static_cast<int>(type));
        return out;
    }
    out.reserve(array_val->size());
    for (const auto& item : *array_val)
    {
        if (item.is_str())
            out.push_back(item.str_val);
        else if (item.is_int())
            out.push_back(std::to_string(item.int_val));
        else
            out.emplace_back();
    }
    return out;
}

std::vector<std::pair<std::string, double>> RedisValue::as_pairs() const
{
    std::vector<std::pair<std::string, double>> out;
    if (type != Type::Array)
    {
        LOG_ERROR("RedisValue::as_pairs on type {}", static_cast<int>(type));
        return out;
    }
    out.reserve(array_val->size() / 2);
    for (size_t i = 0; i + 1 < array_val->size(); i += 2)
        out.emplace_back(array_val->at(i).as_str(), SafeStod(array_val->at(i + 1).as_str()));
    return out;
}

RedisValue RedisValue::FromReply(const redisReply* r)
{
    if (r == nullptr)
        return MakeError("connection closed");

    switch (r->type)
    {
    case REDIS_REPLY_NIL:
        return MakeNil();
    case REDIS_REPLY_INTEGER:
        return MakeInt(r->integer);
    case REDIS_REPLY_DOUBLE:
        return MakeDouble(r->dval);
    case REDIS_REPLY_BOOL:
        return MakeBool(r->integer != 0);
    case REDIS_REPLY_STRING:
    case REDIS_REPLY_STATUS:
    case REDIS_REPLY_VERB:
    case REDIS_REPLY_BIGNUM:
        return MakeStr(std::string(r->str ? r->str : "", r->len));
    case REDIS_REPLY_ERROR:
        return MakeError(std::string(r->str ? r->str : "", r->len));
    case REDIS_REPLY_ARRAY:
    case REDIS_REPLY_SET:
    case REDIS_REPLY_PUSH:
    {
        auto arr = std::make_shared<std::vector<RedisValue>>();
        arr->reserve(r->elements);
        for (size_t i = 0; i < r->elements; ++i)
            arr->push_back(FromReply(r->element[i]));
        return MakeArray(arr);
    }
    case REDIS_REPLY_MAP:
    case REDIS_REPLY_ATTR:
    {
        auto map = std::make_shared<std::vector<std::pair<std::string, RedisValue>>>();
        map->reserve(r->elements / 2);
        for (size_t i = 0; i + 1 < r->elements; i += 2)
        {
            std::string key;
            const redisReply* k = r->element[i];
            if (k != nullptr && k->str != nullptr)
                key.assign(k->str, k->len);
            else if (k != nullptr && k->type == REDIS_REPLY_INTEGER)
                key = std::to_string(k->integer);
            map->emplace_back(std::move(key), FromReply(r->element[i + 1]));
        }
        return MakeMap(map);
    }
    default:
        LOG_ERROR("RedisValue::FromReply unknown reply type {}", static_cast<int>(r->type));
        return MakeNil();
    }
}
