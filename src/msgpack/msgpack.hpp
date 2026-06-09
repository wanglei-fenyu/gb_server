#pragma once

#include <vector>
#include <string>
#include <set>
#include <map>
#include <list>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <math.h>
#include <chrono>
#include <type_traits>
#include <cstring>   // for memcpy

#include <sol/sol.hpp>
#include "gbnet/common/def.h"
#include "gbnet/common/define.h"

#ifdef WIN32
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
#endif

NAMESPACE_BEGIN(gb::msgpack)

class Packer;
class Unpacker;

enum class UnPackErrorType
{
    eNone = 0,
    eOutRange = 1,
};

struct MsgPackBase
{
    virtual void pack(Packer &packer) = 0;
    virtual void unpack(Unpacker &unpacker) = 0;
};

struct UnpackerErrorCategory : public std::error_category
{
    const char *name() const noexcept override
    {
        return "MsgUnpacker";
    }

    std::string message(int ec) const override
    {
        switch (static_cast<UnPackErrorType>(ec))
        {
        case UnPackErrorType::eOutRange:
            return "unpack out range";
        default:
            return "unpack other error";
        }
    }
};

static const UnpackerErrorCategory sUnpackerErrorCategory{};

inline std::error_code make_error_code(UnPackErrorType error_type)
{
    std::error_code ec(static_cast<int>(error_type), sUnpackerErrorCategory);
    return ec;
}

NAMESPACE_END

namespace std
{
    template <>
    struct is_error_code_enum<gb::msgpack::UnPackErrorType> : public true_type
    {
    };
}

NAMESPACE_BEGIN(gb::msgpack)

enum class format_t : uint8_t
{
    nil = 0xc0,
    false_bool = 0xc2,
    true_bool = 0xc3,
    bin8 = 0xc4,
    bin16 = 0xc5,
    bin32 = 0xc6,
    ext8 = 0xc7,
    ext16 = 0xc8,
    ext32 = 0xc9,
    float32 = 0xca,
    float64 = 0xcb,
    uint8 = 0xcc,
    uint16 = 0xcd,
    uint32 = 0xce,
    uint64 = 0xcf,
    int8 = 0xd0,
    int16 = 0xd1,
    int32 = 0xd2,
    int64 = 0xd3,
    fixext1 = 0xd4,
    fixext2 = 0xd5,
    fixext4 = 0xd6,
    fixext8 = 0xd7,
    fixext16 = 0xd8,
    str8 = 0xd9,
    str16 = 0xda,
    str32 = 0xdb,
    array16 = 0xdc,
    array32 = 0xdd,
    map16 = 0xde,
    map32 = 0xdf,
    table = 0xe0
};

// ---------- type traits (unchanged) ----------
template <typename T>
struct is_containe : public std::false_type
{
};

template <typename T, typename Alloc>
struct is_containe<std::vector<T, Alloc>> : public std::true_type
{
};

template <typename T, typename Alloc>
struct is_containe<std::list<T, Alloc>> : public std::true_type
{
};

template <typename T, typename Alloc>
struct is_containe<std::map<T, Alloc>> : public std::true_type
{
};

template <typename T, typename Alloc>
struct is_containe<std::set<T, Alloc>> : public std::true_type
{
};

template <typename T, typename Alloc>
struct is_containe<std::unordered_map<T, Alloc>> : public std::true_type
{
};

template <typename T, typename Alloc>
struct is_containe<std::unordered_set<T, Alloc>> : public std::true_type
{
};

template <typename T>
struct is_stdarray : public std::false_type
{
};

template <typename T, std::size_t N>
struct is_stdarray<std::array<T, N>> : public std::true_type
{
};

template <typename T>
struct is_map : public std::false_type
{
};

template <typename T, typename Alloc>
struct is_map<std::map<T, Alloc>> : public std::true_type
{
};

template <typename T, typename Alloc>
struct is_map<std::unordered_map<T, Alloc>> : public std::true_type
{
};

template <typename T>
struct is_stack_proxy : public std::false_type
{
};

template <>
struct is_stack_proxy<sol::stack_proxy> : public std::true_type
{
};

template <>
struct is_stack_proxy<sol::state_view> : public std::true_type
{
};

template <>
struct is_stack_proxy<sol::state> : public std::true_type
{
};

template <typename T, typename = void>
struct has_pack : std::false_type
{
};

template <typename T>
struct has_pack<T, std::void_t<decltype(&T::pack)>> : std::true_type
{
};

template <typename T>
struct has_constchar : public std::false_type
{
};

template <>
struct has_constchar<const char *> : public std::true_type
{
};

template <size_t N>
struct has_constchar<const char[N]> : public std::true_type
{
};

template <size_t N>
struct has_constchar<char[N]> : public std::true_type
{
};

template <size_t N>
struct has_constchar<const char (&)[N]> : public std::true_type
{
};

template <size_t N>
struct has_constchar<char (&)[N]> : public std::true_type
{
};

template <size_t Idx = 0, typename F, typename... Args>
inline void for_each(std::tuple<Args...> &&tue, F &&f)
{
    if constexpr (Idx < sizeof...(Args))
    {
        f(std::get<Idx>(tue));
        for_each<Idx + 1, F, Args...>(tue, std::move(f));
    }
}

template <size_t Idx = 0, typename F, typename... Args>
inline void for_each(std::tuple<Args...> &tue, F &&f)
{
    if constexpr (Idx < sizeof...(Args))
    {
        f(std::get<Idx>(tue));
        for_each<Idx + 1, F, Args...>(tue, std::move(f));
    }
}

// ---------- Packer ----------
class Packer
{
public:
    template <typename... Args>
    void operator()(const Args &...args)
    {
        (pack_type(std::forward<const Args &>(args)), ...);
    }

    template <typename... Args>
    void process(const Args &...args)
    {
        (pack_type(std::forward<const Args &>(args)), ...);
    }

public:
    const std::vector<uint8_t> &vector() const
    {
        return serializedObj_;
    }

    std::vector<uint8_t> &&move()
    {
        return std::move(serializedObj_);
    }

    void clear()
    {
        serializedObj_.clear();
    }

private:
    template <typename T>
    void pack_type(const T &value)
    {
        if constexpr (std::is_enum<typename std::decay<T>::type>::value)
        {
            pack_type((int32_t)value);
        }
        else if constexpr (is_map<typename std::decay<T>::type>::value)
        {
            pack_map(value);
        }
        else if constexpr (is_containe<typename std::decay<T>::type>::value || is_stdarray<typename std::decay<T>::type>::value)
        {
            pack_array(value);
        }
        else if constexpr (is_stack_proxy<typename std::decay<T>::type>::value)
        {
            pack_stack_proxy(value);
        }
        else if constexpr (has_pack<typename std::decay<T>::type>::value)
        {
            const_cast<T &>(value).pack(*this);
        }
        else if constexpr (std::is_same_v<typename std::decay<T>::type, const char *> || std::is_same_v<typename std::decay<T>::type, std::string_view> || has_constchar<T>::value)
        {
            pack_type(std::string(value));
        }
        else
        {
        }
    }

    template <typename T>
    void pack_type(const std::chrono::time_point<T> &value)
    {
        pack_type(value.time_since_epoch().count());
    }

    template <typename T>
    void pack_array(const T &array)
    {
        const size_t size = array.size();

        if (size <= 15)
        {
            serializedObj_.emplace_back(uint8_t(0x90 | size));
        }
        else if (size <= 0xffff)
        {
            serializedObj_.emplace_back((uint8_t)format_t::array16);
            for (auto i = sizeof(uint16_t); i > 0; --i)
            {
                serializedObj_.emplace_back(uint8_t(size >> (8 * (i - 1)) & 0xff));
            }
        }
        else if (size <= 0xffffffff)
        {
            serializedObj_.emplace_back((uint8_t)format_t::array32);
            for (auto i = sizeof(uint32_t); i > 0; --i)
            {
                serializedObj_.emplace_back(uint8_t(size >> (8 * (i - 1)) & 0xff));
            }
        }
        else
        {
            return;
        }

        for (const auto &item : array)
        {
            pack_type(item);
        }
    }

    template <typename T>
    void pack_map(const T &map)
    {
        const size_t size = map.size();

        if (size <= 15)
        {
            serializedObj_.emplace_back(uint8_t(0x80 | size));
        }
        else if (size <= 0xffff)
        {
            serializedObj_.emplace_back((uint8_t)format_t::map16);
            for (auto i = sizeof(uint16_t); i > 0; --i)
            {
                serializedObj_.emplace_back(uint8_t(size >> (8 * (i - 1)) & 0xff));
            }
        }
        else if (size <= 0xffffffff)
        {
            serializedObj_.emplace_back((uint8_t)format_t::map32);
            for (auto i = sizeof(uint32_t); i > 0; --i)
            {
                serializedObj_.emplace_back(uint8_t(size >> (8 * (i - 1)) & 0xff));
            }
        }
        else
        {
            return;
        }

        for (auto &item : map)
        {
            pack_type(std::get<0>(item));
            pack_type(std::get<1>(item));
        }
    }

    template <typename T>
    void pack_table(const T &table)
    {
        serializedObj_.emplace_back((uint8_t)format_t::table);
        size_t index = serializedObj_.size();
        size_t count = 0;
        size_t offset = 0;
        for (int i = sizeof(uint32_t); i > 0; --i)
        {
            serializedObj_.emplace_back(uint8_t(0));
        }
        for (auto it = table.begin(); it != table.end(); ++it)
        {
            count++;
            auto &p = *it;
            pack_stack_proxy(p.first);
            pack_stack_proxy(p.second);
        }

        for (auto i = sizeof(uint32_t); i > 0; --i)
        {
            serializedObj_[index + offset] = (uint8_t(count >> (8 * (i - 1)) & 0xff));
            offset++;
        }
    }

    template <typename T>
    void pack_stack_proxy(const T &obj)
    {
        auto type = obj.get_type();
        switch (type)
        {
        case sol::type::lua_nil:
            pack_type(nullptr);
            break;
        case sol::type::number:
            obj.push();
            if (lua_isinteger(obj.lua_state(), -1))
            {
                pack_type(obj.template as<int64_t>());
            }
            else
            {
                pack_type(obj.template as<double>());
            }
            lua_pop(obj.lua_state(), 1);
            break;
        case sol::type::string:
            pack_type(obj.template as<std::string>());
            break;
        case sol::type::boolean:
            pack_type(obj.template as<bool>());
            break;
        case sol::type::table:
            obj.push();
            pack_table(sol::table(obj.lua_state(), -1));
            lua_pop(obj.lua_state(), 1);
            break;
        case sol::type::userdata:
            if (obj.template is<MsgPackBase>())
            {
                MsgPackBase &msg_base = obj.template as<MsgPackBase>();
                msg_base.pack(*this);
            }
            break;
        default:
            break;
        }
    }

private:
    std::vector<uint8_t> serializedObj_;
};

// ----- pack_type specializations (with fix types and no bitset) -----

template <>
inline void Packer::pack_type(const int8_t &value)
{
    if (value >= -32 && value <= -1)
    {
        serializedObj_.emplace_back(uint8_t(0xe0 + 32 + value));
    }
    else if (value >= 0 && value <= 0x7f)
    {
        serializedObj_.emplace_back(uint8_t(value));
    }
    else
    {
        serializedObj_.emplace_back((uint8_t)format_t::int8);
        serializedObj_.emplace_back(static_cast<uint8_t>(value));
    }
}

template <>
inline void Packer::pack_type(const int16_t &value)
{
    if (value <= std::numeric_limits<int8_t>::max() && value >= std::numeric_limits<int8_t>::min())
    {
        pack_type(int8_t(value));
    }
    else
    {
        serializedObj_.emplace_back((uint8_t)format_t::int16);
        auto serialize_val = static_cast<uint16_t>(value);
        for (int i = sizeof(value); i > 0; --i)
        {
            serializedObj_.emplace_back(uint8_t(serialize_val >> (8 * (i - 1)) & 0xff));
        }
    }
}

template <>
inline void Packer::pack_type(const int32_t &value)
{
    if (value <= std::numeric_limits<int16_t>::max() && value >= std::numeric_limits<int16_t>::min())
    {
        pack_type((int16_t)value);
    }
    else
    {
        serializedObj_.emplace_back((uint8_t)format_t::int32);
        auto serialize_val = static_cast<uint32_t>(value);
        for (int i = sizeof(value); i > 0; --i)
        {
            serializedObj_.emplace_back(uint8_t(serialize_val >> (8 * (i - 1)) & 0xff));
        }
    }
}

template <>
inline void Packer::pack_type(const int64_t &value)
{
    if (value <= std::numeric_limits<int32_t>::max() && value >= std::numeric_limits<int32_t>::min())
    {
        pack_type((int32_t)value);
    }
    else
    {
        serializedObj_.emplace_back((uint8_t)format_t::int64);
        auto serialize_val = static_cast<uint64_t>(value);
        for (int i = sizeof(value); i > 0; --i)
        {
            serializedObj_.emplace_back(uint8_t(serialize_val >> (8 * (i - 1)) & 0xff));
        }
    }
}

template <>
inline void Packer::pack_type(const uint8_t &value)
{
    if (value <= 0x7f)
    {
        serializedObj_.emplace_back(value);
    }
    else
    {
        serializedObj_.emplace_back((uint8_t)format_t::uint8);
        serializedObj_.emplace_back(value);
    }
}

template <>
inline void Packer::pack_type(const uint16_t &value)
{
    if (value > std::numeric_limits<uint8_t>::max())
    {
        serializedObj_.emplace_back((uint8_t)format_t::uint16);
        for (auto i = sizeof(value); i > 0; --i)
        {
            serializedObj_.emplace_back(uint8_t(value >> (8 * (i - 1)) & 0xff));
        }
    }
    else
    {
        pack_type(uint8_t(value));
    }
}

template <>
inline void Packer::pack_type(const uint32_t &value)
{
    if (value > std::numeric_limits<uint16_t>::max())
    {
        serializedObj_.emplace_back((uint8_t)format_t::uint32);
        for (auto i = sizeof(value); i > 0U; --i)
        {
            serializedObj_.emplace_back(uint8_t(value >> (8 * (i - 1)) & 0xff));
        }
    }
    else
    {
        pack_type(uint16_t(value));
    }
}

template <>
inline void Packer::pack_type(const uint64_t &value)
{
    if (value > std::numeric_limits<uint32_t>::max())
    {
        serializedObj_.emplace_back((uint8_t)format_t::uint64);
        for (auto i = sizeof(value); i > 0U; --i)
        {
            serializedObj_.emplace_back(uint8_t(value >> (8 * (i - 1)) & 0xff));
        }
    }
    else
    {
        pack_type(uint32_t(value));
    }
}

template <>
inline void Packer::pack_type(const float &value)
{
    int32_t i = (int32_t)value;
    if (float(i) == value)
    {
        pack_type(i);
    }
    else
    {
        serializedObj_.emplace_back((uint8_t)format_t::float32);
        uint8_t *s = (uint8_t *)&value;
        for (auto i = 0; i < sizeof(float); ++i)
        {
            serializedObj_.emplace_back(s[i]);
        }
    }
}

template <>
inline void Packer::pack_type(const double &value)
{
    int64_t i = (int64_t)value;
    if (double(i) == value)
    {
        pack_type(i);
    }
    else
    {
        serializedObj_.emplace_back((uint8_t)format_t::float64);
        uint8_t *s = (uint8_t *)&value;
        for (auto i = 0; i < sizeof(double); ++i)
        {
            serializedObj_.emplace_back(s[i]);
        }
    }
}

template <>
inline void Packer::pack_type(const std::nullptr_t &none)
{
    serializedObj_.emplace_back((uint8_t)format_t::nil);
}

template <>
inline void Packer::pack_type(const bool &value)
{
    if (value)
        serializedObj_.emplace_back((uint8_t)format_t::true_bool);
    else
        serializedObj_.emplace_back((uint8_t)format_t::false_bool);
}

template <>
inline void Packer::pack_type(const std::string &str)
{
    const size_t len = str.size();

    if (len <= 31)
    {
        serializedObj_.emplace_back(uint8_t(0xa0 | len));
    }
    else if (len <= 0xff)
    {
        serializedObj_.emplace_back((uint8_t)format_t::str8);
        serializedObj_.emplace_back(uint8_t(len));
    }
    else if (len <= 0xffff)
    {
        serializedObj_.emplace_back((uint8_t)format_t::str16);
        for (auto i = sizeof(uint16_t); i > 0; --i)
        {
            serializedObj_.emplace_back(uint8_t(len >> (8 * (i - 1)) & 0xff));
        }
    }
    else if (len <= 0xffffffff)
    {
        serializedObj_.emplace_back((uint8_t)format_t::str32);
        for (auto i = sizeof(uint32_t); i > 0; --i)
        {
            serializedObj_.emplace_back(uint8_t(len >> (8 * (i - 1)) & 0xff));
        }
    }
    else
    {
        return;
    }

    for (const auto &c : str)
    {
        serializedObj_.emplace_back(uint8_t(c));
    }
}

template <>
inline void Packer::pack_type(const std::vector<uint8_t> &data)
{
    if (data.size() < std::numeric_limits<uint8_t>::max())
    {
        serializedObj_.emplace_back((uint8_t)format_t::bin8);
        serializedObj_.emplace_back((uint8_t)data.size());
    }
    else if (data.size() < std::numeric_limits<uint16_t>::max())
    {
        serializedObj_.emplace_back((uint8_t)format_t::bin16);
        for (int i = sizeof(uint16_t); i > 0; --i)
        {
            serializedObj_.emplace_back(uint8_t(data.size() >> (8 * (i - 1)) & 0xff));
        }
    }
    else if (data.size() < std::numeric_limits<uint32_t>::max())
    {
        serializedObj_.emplace_back((uint8_t)format_t::bin32);
        for (int i = sizeof(uint32_t); i > 0; --i)
        {
            serializedObj_.emplace_back(uint8_t(data.size() >> (8 * (i - 1)) & 0xff));
        }
    }
    else
    {
        return;
    }

    for (const auto &b : data)
    {
        serializedObj_.emplace_back(b);
    }
}

// ----- free pack functions (unchanged) -----

template <typename... Args>
std::vector<uint8_t> pack(Args &...args)
{
    auto packer = Packer{};
    auto args_tuple = std::forward_as_tuple(std::forward<Args>(args)...);
    for_each(args_tuple, [&](auto &obj)
             {
                 if constexpr (std::is_same<typename std::decay<decltype(obj)>::type, sol::variadic_args>::value)
                 {
                     for (int i = 0; i < obj.size(); i++)
                         packer(obj[i]);
                 }
                 else if constexpr (std::is_same<typename std::decay<decltype(obj)>::type, sol::protected_function_result>::value)
                 {
                     for (int i = 0; i < obj.return_count(); i++)
                         packer(obj[i]);
                 }
                 else if constexpr (has_pack<typename std::decay<decltype(obj)>::type>::value)
                 {
                     obj.pack(packer);
                 }
                 else
                 {
                     packer(obj);
                 }
             });
    return std::move(packer.move());
}

template <typename... Args>
std::vector<uint8_t> pack(Args &&...args)
{
    auto packer = Packer{};
    auto args_tuple = std::forward_as_tuple(std::forward<Args>(args)...);
    for_each(args_tuple, [&](auto &obj)
             {
                 if constexpr (std::is_same<typename std::decay<decltype(obj)>::type, sol::variadic_args>::value)
                 {
                     for (int i = 0; i < obj.size(); i++)
                         packer(obj[i]);
                 }
                 else if constexpr (std::is_same<typename std::decay<decltype(obj)>::type, sol::protected_function_result>::value)
                 {
                     for (int i = 0; i < obj.return_count(); i++)
                         packer(obj[i]);
                 }
                 else if constexpr (has_pack<typename std::decay<decltype(obj)>::type>::value)
                 {
                     obj.pack(packer);
                 }
                 else
                 {
                     packer(obj);
                 }
             });
    return std::move(packer.move());
}

template <typename... Args>
std::vector<uint8_t> pack(std::tuple<Args...> &args)
{
    auto packer = Packer{};
    for_each(std::forward<std::tuple<Args...>>(args), [&](auto &obj)
             {
                 if constexpr (std::is_same<typename std::decay<decltype(obj)>::type, sol::variadic_args>::value)
                 {
                     for (int i = 0; i < obj.size(); i++)
                         packer(obj[i]);
                 }
                 else if constexpr (std::is_same<typename std::decay<decltype(obj)>::type, sol::protected_function_result>::value)
                 {
                     for (int i = 0; i < obj.return_count(); i++)
                         packer(obj[i]);
                 }
                 else if constexpr (has_pack<typename std::decay<decltype(obj)>::type>::value)
                 {
                     obj.pack(packer);
                 }
                 else
                 {
                     packer(obj);
                 }
             });
    return std::move(packer.move());
}

template <typename... Args>
std::vector<uint8_t> pack(std::tuple<Args...> &&args)
{
    auto packer = Packer{};
    for_each(std::forward<std::tuple<Args...>>(args), [&](auto &obj)
             {
                 if constexpr (std::is_same<typename std::decay<decltype(obj)>::type, sol::variadic_args>::value)
                 {
                     for (int i = 0; i < obj.size(); i++)
                         packer(obj[i]);
                 }
                 else if constexpr (std::is_same<typename std::decay<decltype(obj)>::type, sol::protected_function_result>::value)
                 {
                     for (int i = 0; i < obj.return_count(); i++)
                         packer(obj[i]);
                 }
                 else if constexpr (has_pack<typename std::decay<decltype(obj)>::type>::value)
                 {
                     obj.pack(packer);
                 }
                 else
                 {
                     packer(obj);
                 }
             });
    return std::move(packer.move());
}

std::vector<uint8_t> pack(sol::variadic_args &args);
std::vector<uint8_t> pack(sol::variadic_args &&args);
std::vector<uint8_t> pack(sol::protected_function_result &args);
std::vector<uint8_t> pack(sol::protected_function_result &&args);

// ---------- Unpacker ----------
class Unpacker
{
public:
    Unpacker() : dataPointer_(nullptr), dataEnd_(nullptr) {}
    Unpacker(const uint8_t *dataStart, std::size_t size) : dataPointer_(dataStart), dataEnd_(dataStart + size) {}

    std::error_code ec{};

public:
    template <typename... Types>
    void operator()(Types &...args)
    {
        (unpack_type(std::forward<Types &>(args)), ...);
    }

    template <typename... Types>
    void process(Types &...args)
    {
        (unpack_type(std::forward<Types &>(args)), ...);
    }

    void set_data(const uint8_t *start, std::size_t size)
    {
        dataPointer_ = start;
        dataEnd_ = start + size;
    }

    bool empty()
    {
        return dataEnd_ - dataPointer_ <= 0;
    }

private:
    uint8_t safe_data()
    {
        if (dataPointer_ < dataEnd_)
            return *dataPointer_;
        ec = make_error_code(UnPackErrorType::eOutRange);
        return 0;
    }

    void safe_incremen(std::size_t size = 1)
    {
        if (dataPointer_ + size <= dataEnd_)
            dataPointer_ += size;
        else
        {
            dataPointer_ = dataEnd_;
            ec = make_error_code(UnPackErrorType::eOutRange);
        }
    }

    // ------------------------------------------------------------
    //  Optimized integer reading: no bitset, just byte assembly
    // ------------------------------------------------------------
    template <typename Int>
    void read_be_unsigned(Int &out)
    {
        static_assert(std::is_unsigned_v<Int>);
        out = 0;
        for (size_t i = 0; i < sizeof(Int); ++i)
        {
            out = (out << 8) | safe_data();
            safe_incremen();
        }
    }

    template <typename Int>
    void read_be_signed(Int &out)
    {
        using Unsigned = typename std::make_unsigned<Int>::type;
        Unsigned u = 0;
        for (size_t i = 0; i < sizeof(Int); ++i)
        {
            u = (u << 8) | safe_data();
            safe_incremen();
        }
        // Two's complement conversion
        constexpr Unsigned sign_mask = Unsigned(1) << (sizeof(Int) * 8 - 1);
        if (u & sign_mask)
            out = -static_cast<Int>(~u + 1);
        else
            out = static_cast<Int>(u);
    }

    template <class T>
    void unpack_type(T &value)
    {
        if constexpr (std::is_enum<typename std::decay<T>::type>::value)
        {
            int32_t v = 0;
            unpack_type(v);
            value = (T)v;
        }
        else if constexpr (is_map<typename std::decay<T>::type>::value)
        {
            unpack_map(value);
        }
        else if constexpr (is_containe<typename std::decay<T>::type>::value)
        {
            unpack_array(value);
        }
        else if constexpr (is_stdarray<typename std::decay<T>::type>::value)
        {
            unpack_stdarray(value);
        }
        else if constexpr (is_stack_proxy<typename std::decay<T>::type>::value)
        {
            unpack_stack_proxy(value);
        }
        else if constexpr (std::is_arithmetic_v<typename std::decay<T>::type>)
        {
            unpack_number(value);
        }
        else
        {
            if constexpr (has_pack<typename std::decay<T>::type>::value)
            {
                value.unpack(*this);
            }
        }
    }

    template <class Clock, class Duration>
    void unpack_type(std::chrono::time_point<Clock, Duration> &value)
    {
        using RepType = typename std::chrono::time_point<Clock, Duration>::rep;
        using DurationType = Duration;
        using TimepointType = typename std::chrono::time_point<Clock, Duration>;
        auto placeholder = RepType{};
        unpack_type(placeholder);
        value = TimepointType(DurationType(placeholder));
    }

    template <class T>
    void unpack_array(T &array)
    {
        using ValueType = typename T::value_type;
        uint8_t byte = safe_data();

        if (byte >= 0x90 && byte <= 0x9f)
        {
            std::size_t array_size = byte & 0x0f;
            safe_incremen();
            if constexpr (requires { array.reserve(std::size_t{}); })
                if (array_size)
                    array.reserve(array_size);
            for (std::size_t i = 0; i < array_size; ++i)
            {
                ValueType val{};
                unpack_type(val);
                array.emplace_back(val);
            }
        }
        else if (byte == (uint8_t)format_t::array32)
        {
            safe_incremen();
            std::size_t array_size = 0;
            read_be_unsigned(array_size);
            if constexpr (requires { array.reserve(std::size_t{}); })
                if (array_size)
                    array.reserve(array_size);
            for (std::size_t i = 0; i < array_size; ++i)
            {
                ValueType val{};
                unpack_type(val);
                array.emplace_back(val);
            }
        }
        else if (byte == (uint8_t)format_t::array16)
        {
            safe_incremen();
            std::size_t array_size = 0;
            read_be_unsigned(array_size);
            if constexpr (requires { array.reserve(std::size_t{}); })
                if (array_size)
                    array.reserve(array_size);
            for (std::size_t i = 0; i < array_size; ++i)
            {
                ValueType val{};
                unpack_type(val);
                array.emplace_back(val);
            }
        }
        else if (byte == (uint8_t)format_t::table)
        {
            safe_incremen();
            std::size_t table_size = 0;
            read_be_unsigned(table_size);
            if constexpr (requires { std::declval<T &>()[std::declval<int64_t>()]; })
            {
                if (table_size > 0)
                    array.resize(table_size);
                for (std::size_t i = 0; i < table_size; ++i)
                {
                    int64_t key{};
                    ValueType val{};
                    unpack_type(key);
                    unpack_type(val);
                    array[key - 1] = std::move(val);
                }
            }
        }
        else
        {
            ec = make_error_code(UnPackErrorType::eOutRange);
        }
    }

    template <typename T>
    void unpack_stdarray(T &array)
    {
        using ValueType = typename T::value_type;
        auto vec = std::vector<ValueType>{};
        unpack_array(vec);
        std::copy(vec.begin(), vec.end(), array.begin());
    }

    template <typename T>
    void unpack_map(T &map)
    {
        using KeyType = typename T::key_type;
        using MappedType = typename T::mapped_type;
        uint8_t byte = safe_data();

        if (byte >= 0x80 && byte <= 0x8f)
        {
            std::size_t map_size = byte & 0x0f;
            safe_incremen();
            if constexpr (requires { map.reserve(std::size_t{}); })
                if (map_size)
                    map.reserve(map_size);
            for (std::size_t i = 0; i < map_size; ++i)
            {
                KeyType key{};
                MappedType val{};
                unpack_type(key);
                unpack_type(val);
                map.insert_or_assign(key, val);
            }
        }
        else if (byte == (uint8_t)format_t::map32 || byte == (uint8_t)format_t::table)
        {
            safe_incremen();
            std::size_t map_size = 0;
            read_be_unsigned(map_size);
            if constexpr (requires { map.reserve(std::size_t{}); })
                if (map_size)
                    map.reserve(map_size);
            for (std::size_t i = 0; i < map_size; ++i)
            {
                KeyType key{};
                MappedType val{};
                unpack_type(key);
                unpack_type(val);
                map.insert_or_assign(key, val);
            }
        }
        else if (byte == (uint8_t)format_t::map16)
        {
            safe_incremen();
            std::size_t map_size = 0;
            read_be_unsigned(map_size);
            if constexpr (requires { map.reserve(std::size_t{}); })
                if (map_size)
                    map.reserve(map_size);
            for (std::size_t i = 0; i < map_size; ++i)
            {
                KeyType key{};
                MappedType val{};
                unpack_type(key);
                unpack_type(val);
                map.insert_or_assign(key, val);
            }
        }
    }

    template <typename T>
    void unpack_stack_proxy(T &state)
    {
        uint8_t byte = safe_data();
        switch (byte)
        {
        case (uint8_t)format_t::nil:
        {
            lua_pushnil(state.lua_state());
            safe_incremen();
            break;
        }
        case (uint8_t)format_t::false_bool:
        {
            lua_pushboolean(state.lua_state(), false);
            safe_incremen();
            break;
        }
        case (uint8_t)format_t::true_bool:
        {
            lua_pushboolean(state.lua_state(), true);
            safe_incremen();
            break;
        }
        case (uint8_t)format_t::bin8:
        case (uint8_t)format_t::bin16:
        case (uint8_t)format_t::bin32:
        {
            std::vector<uint8_t> value;
            unpack_type(value);
            lua_pushlstring(state.lua_state(), (char *)(value.data()), value.size());
            break;
        }
        case (uint8_t)format_t::float32:
        {
            float value = 0.f;
            unpack_type(value);
            lua_pushnumber(state.lua_state(), value);
            break;
        }
        case (uint8_t)format_t::float64:
        {
            double value = 0.f;
            unpack_type(value);
            lua_pushnumber(state.lua_state(), value);
            break;
        }
        case (uint8_t)format_t::uint8:
        case (uint8_t)format_t::uint16:
        case (uint8_t)format_t::uint32:
        case (uint8_t)format_t::uint64:
        {
            uint64_t value = 0;
            unpack_type(value);
            lua_pushinteger(state.lua_state(), value);
            break;
        }
        case (uint8_t)format_t::int8:
        case (uint8_t)format_t::int16:
        case (uint8_t)format_t::int32:
        case (uint8_t)format_t::int64:
        {
            int64_t value = 0;
            unpack_type(value);
            lua_pushinteger(state.lua_state(), value);
            break;
        }
        case (uint8_t)format_t::str8:
        case (uint8_t)format_t::str16:
        case (uint8_t)format_t::str32:
        {
            std::string value;
            unpack_type(value);
            lua_pushlstring(state.lua_state(), value.c_str(), value.size());
            break;
        }
        case (uint8_t)format_t::array16:
        case (uint8_t)format_t::array32:
        case (uint8_t)format_t::map16:
        case (uint8_t)format_t::map32:
        case (uint8_t)format_t::table:
        {
            unpack_table(state);
            break;
        }
        default:
        {
            if (byte >= 0x00 && byte <= 0x7f)
            {
                // positive fixint
                uint8_t value = byte;
                safe_incremen();
                lua_pushinteger(state.lua_state(), value);
            }
            else if (byte >= 0xe0 && byte <= 0xff)
            {
                // negative fixint
                int8_t value = static_cast<int8_t>(byte);
                safe_incremen();
                lua_pushinteger(state.lua_state(), value);
            }
            else if (byte >= 0x90 && byte <= 0x9f)
            {
                // fixarray
                unpack_table(state);
            }
            else if (byte >= 0x80 && byte <= 0x8f)
            {
                // fixmap
                unpack_table(state);
            }
            else if (byte >= 0xa0 && byte <= 0xbf)
            {
                // fixstr
                std::size_t strSize = byte & 0b00011111;
                safe_incremen();
                if (dataPointer_ + strSize <= dataEnd_)
                {
                    lua_pushlstring(state.lua_state(), (char *)(dataPointer_), strSize);
                    safe_incremen(strSize);
                }
                else
                {
                    ec = UnPackErrorType::eOutRange;
                }
            }
            else
            {
                // unknown
                ec = make_error_code(UnPackErrorType::eOutRange);
            }
            break;
        }
        }
    }

    template <typename T>
    void unpack_table(T &state)
    {
        uint8_t byte = safe_data();
        if (byte == (uint8_t)format_t::array32)
        {
            lua_newtable(state.lua_state());
            safe_incremen();
            std::size_t array_size = 0;
            for (auto i = sizeof(uint32_t); i > 0; --i)
            {
                array_size += uint32_t(safe_data()) << 8 * (i - 1);
                safe_incremen();
            }
            for (std::size_t i = 0; i < array_size; ++i)
            {
                unpack_stack_proxy(state);
                lua_rawseti(state.lua_state(), -2, i + 1);
            }
        }
        else if (byte == (uint8_t)format_t::array16)
        {
            lua_newtable(state.lua_state());
            safe_incremen();
            std::size_t array_size = 0;
            for (auto i = sizeof(uint16_t); i > 0; --i)
            {
                array_size += uint16_t(safe_data()) << 8 * (i - 1);
                safe_incremen();
            }
            for (std::size_t i = 0; i < array_size; ++i)
            {
                unpack_stack_proxy(state);
                lua_rawseti(state.lua_state(), -2, i + 1);
            }
        }
        else if (byte == (uint8_t)format_t::map32 || byte == (uint8_t)format_t::table)
        {
            lua_newtable(state.lua_state());
            safe_incremen();
            std::size_t map_size = 0;
            for (auto i = sizeof(uint32_t); i > 0; --i)
            {
                map_size += uint32_t(safe_data()) << 8 * (i - 1);
                safe_incremen();
            }
            for (std::size_t i = 0; i < map_size; ++i)
            {
                unpack_stack_proxy(state);
                unpack_stack_proxy(state);
                lua_rawset(state.lua_state(), -3);
            }
        }
        else if (byte == (uint8_t)format_t::map16)
        {
            lua_newtable(state.lua_state());
            safe_incremen();
            std::size_t map_size = 0;
            for (auto i = sizeof(uint16_t); i > 0; --i)
            {
                map_size += uint16_t(safe_data()) << 8 * (i - 1);
                safe_incremen();
            }
            for (std::size_t i = 0; i < map_size; ++i)
            {
                unpack_stack_proxy(state);
                unpack_stack_proxy(state);
                lua_rawset(state.lua_state(), -3);
            }
        }
        else if (byte >= 0x90 && byte <= 0x9f)
        {
            // fixarray
            lua_newtable(state.lua_state());
            std::size_t array_size = byte & 0x0f;
            safe_incremen();
            for (std::size_t i = 0; i < array_size; ++i)
            {
                unpack_stack_proxy(state);
                lua_rawseti(state.lua_state(), -2, i + 1);
            }
        }
        else if (byte >= 0x80 && byte <= 0x8f)
        {
            // fixmap
            lua_newtable(state.lua_state());
            std::size_t map_size = byte & 0x0f;
            safe_incremen();
            for (std::size_t i = 0; i < map_size; ++i)
            {
                unpack_stack_proxy(state);
                unpack_stack_proxy(state);
                lua_rawset(state.lua_state(), -3);
            }
        }
        else
        {
            // unknown
            ec = make_error_code(UnPackErrorType::eOutRange);
        }
    }

    // Optimized number unpacker: uses read_be_unsigned/read_be_signed
    template <typename T>
    void unpack_number(T &val)
    {
        if constexpr (std::is_arithmetic<T>::value)
        {
            uint8_t byte = safe_data();

            // fixint positive
            if (byte <= 0x7f)
            {
                val = static_cast<T>(byte);
                safe_incremen();
                return;
            }
            // fixint negative
            if (byte >= 0xe0)
            {
                val = static_cast<T>(static_cast<int8_t>(byte));
                safe_incremen();
                return;
            }

            // typed markers
            if (byte == (uint8_t)format_t::uint64)
            {
                safe_incremen();
                uint64_t u;
                read_be_unsigned(u);
                val = static_cast<T>(u);
            }
            else if (byte == (uint8_t)format_t::uint32)
            {
                safe_incremen();
                uint32_t u;
                read_be_unsigned(u);
                val = static_cast<T>(u);
            }
            else if (byte == (uint8_t)format_t::uint16)
            {
                safe_incremen();
                uint16_t u;
                read_be_unsigned(u);
                val = static_cast<T>(u);
            }
            else if (byte == (uint8_t)format_t::uint8)
            {
                safe_incremen();
                uint8_t u;
                read_be_unsigned(u);
                val = static_cast<T>(u);
            }
            else if (byte == (uint8_t)format_t::int64)
            {
                safe_incremen();
                int64_t i;
                read_be_signed(i);
                val = static_cast<T>(i);
            }
            else if (byte == (uint8_t)format_t::int32)
            {
                safe_incremen();
                int32_t i;
                read_be_signed(i);
                val = static_cast<T>(i);
            }
            else if (byte == (uint8_t)format_t::int16)
            {
                safe_incremen();
                int16_t i;
                read_be_signed(i);
                val = static_cast<T>(i);
            }
            else if (byte == (uint8_t)format_t::int8)
            {
                safe_incremen();
                int8_t i;
                read_be_signed(i);
                val = static_cast<T>(i);
            }
            else if (byte == (uint8_t)format_t::float32)
            {
                safe_incremen();
                float f;
                // fast memcpy from stream
                std::memcpy(&f, dataPointer_, sizeof(float));
                safe_incremen(sizeof(float));
                val = static_cast<T>(f);
            }
            else if (byte == (uint8_t)format_t::float64)
            {
                safe_incremen();
                double d;
                std::memcpy(&d, dataPointer_, sizeof(double));
                safe_incremen(sizeof(double));
                val = static_cast<T>(d);
            }
            else
            {
                ec = make_error_code(UnPackErrorType::eOutRange);
            }
        }
    }

private:
    const uint8_t *dataPointer_;
    const uint8_t *dataEnd_;
};

// ----- unpack_type specializations (delegating to unpack_number) -----

template <>
inline void Unpacker::unpack_type(int8_t &value)
{
    unpack_number(value);
}
template <>
inline void Unpacker::unpack_type(int16_t &value)
{
    unpack_number(value);
}
template <>
inline void Unpacker::unpack_type(int32_t &value)
{
    unpack_number(value);
}
template <>
inline void Unpacker::unpack_type(int64_t &value)
{
    unpack_number(value);
}
template <>
inline void Unpacker::unpack_type(uint8_t &value)
{
    unpack_number(value);
}
template <>
inline void Unpacker::unpack_type(uint16_t &value)
{
    unpack_number(value);
}
template <>
inline void Unpacker::unpack_type(uint32_t &value)
{
    unpack_number(value);
}
template <>
inline void Unpacker::unpack_type(uint64_t &value)
{
    unpack_number(value);
}
template <>
inline void Unpacker::unpack_type(std::nullptr_t & /*value*/)
{
    safe_incremen();
}
template <>
inline void Unpacker::unpack_type(bool &value)
{
    if (safe_data() == (uint8_t)format_t::false_bool)
    {
        value = false;
        safe_incremen();
    }
    else if (safe_data() == (uint8_t)format_t::true_bool)
    {
        value = true;
        safe_incremen();
    }
    else
    {
        unpack_number(value);
    }
}
template <>
inline void Unpacker::unpack_type(float &value)
{
    unpack_number(value);
}
template <>
inline void Unpacker::unpack_type(double &value)
{
    unpack_number(value);
}
template <>
inline void Unpacker::unpack_type(std::string &value)
{
    std::size_t strSize = 0;
    uint8_t byte = safe_data();

    if (byte >= 0xa0 && byte <= 0xbf)
    {
        strSize = byte & 0x1f;
        safe_incremen();
    }
    else if (byte == (uint8_t)format_t::str32)
    {
        safe_incremen();
        read_be_unsigned(strSize);
    }
    else if (byte == (uint8_t)format_t::str16)
    {
        safe_incremen();
        read_be_unsigned(strSize);
    }
    else if (byte == (uint8_t)format_t::str8)
    {
        safe_incremen();
        uint8_t len;
        read_be_unsigned(len);
        strSize = len;
    }
    else
    {
        ec = make_error_code(UnPackErrorType::eOutRange);
        return;
    }

    if (dataPointer_ + strSize <= dataEnd_)
    {
        value = std::string{dataPointer_, dataPointer_ + strSize};
        safe_incremen(strSize);
    }
    else
    {
        ec = make_error_code(UnPackErrorType::eOutRange);
    }
}
template <>
inline void Unpacker::unpack_type(std::vector<uint8_t> &value)
{
    std::size_t binSize = 0;
    uint8_t byte = safe_data();

    if (byte == (uint8_t)format_t::bin32)
    {
        safe_incremen();
        read_be_unsigned(binSize);
    }
    else if (byte == (uint8_t)format_t::bin16)
    {
        safe_incremen();
        read_be_unsigned(binSize);
    }
    else if (byte == (uint8_t)format_t::bin8)
    {
        safe_incremen();
        uint8_t len;
        read_be_unsigned(len);
        binSize = len;
    }
    else
    {
        ec = make_error_code(UnPackErrorType::eOutRange);
        return;
    }

    if (dataPointer_ + binSize <= dataEnd_)
    {
        value = std::vector<uint8_t>{dataPointer_, dataPointer_ + binSize};
        safe_incremen(binSize);
    }
    else
    {
        ec = make_error_code(UnPackErrorType::eOutRange);
    }
}

// ----- free unpack functions (unchanged) -----

template <typename... Args>
std::tuple<Args...> unpack(const uint8_t *dataStart, const std::size_t size, std::error_code &ec)
{
    auto unpacker = Unpacker(dataStart, size);
    std::tuple<Args...> objs;
    for_each(objs, [&](auto &obj)
             {
                 if constexpr (has_pack<typename std::decay<decltype(obj)>::type>::value)
                     obj.unpack(unpacker);
                 else
                     unpacker(obj);
             });
    ec = unpacker.ec;
    return objs;
}

template <class... Args>
std::tuple<Args...> unpack(const uint8_t *dataStart, const std::size_t size)
{
    std::error_code ec{};
    return unpack<Args...>(dataStart, size, ec);
}

template <class... Args>
std::tuple<Args...> unpack(const std::vector<uint8_t> &data)
{
    std::error_code ec;
    return unpack<Args...>(data.data(), data.size(), ec);
}

template <class... Args>
std::tuple<Args...> unpack(const std::vector<uint8_t> &data, std::error_code &ec)
{
    return unpack<Args...>(data.data(), data.size(), ec);
}

sol::variadic_args unpack(sol::state_view &state, const uint8_t *dataStart, const std::size_t size, std::error_code &ec);
sol::variadic_args unpack(sol::state_view &state, const uint8_t *dataStart, const std::size_t size);
sol::variadic_args unpack(sol::state_view &state, const std::vector<uint8_t> &data, std::error_code &ec);
sol::variadic_args unpack(sol::state_view &state, const std::vector<uint8_t> &data);

NAMESPACE_END

#define PACKER_BASE gb::msgpack::MsgPackBase

#define REGISTER_PACKER(...)                                   \
public:                                                        \
    virtual void pack(gb::msgpack::Packer &packer)             \
    {                                                          \
        packer(__VA_ARGS__);                                   \
    }                                                          \
    virtual void unpack(gb::msgpack::Unpacker &unpacker)       \
    {                                                          \
        unpacker(__VA_ARGS__);                                 \
    }