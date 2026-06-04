//
// msgpack_test.cpp — comprehensive msgpack pack/unpack unit tests
//
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "msgpack/msgpack.hpp"

#include <array>
#include <list>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <cstring>

// ============================================================
// Test helper
// ============================================================
template <typename T>
static T roundtrip(const T& val)
{
    auto data = gb::msgpack::pack(val);
    auto [result] = gb::msgpack::unpack<T>(data);
    return result;
}

template <typename T>
static T roundtrip_rvalue(T&& val)
{
    auto data = gb::msgpack::pack(std::forward<T>(val));
    auto [result] = gb::msgpack::unpack<std::decay_t<T>>(data);
    return result;
}

// ============================================================
// Custom object with REGISTER_PACKER
// ============================================================
struct TestPackerObj : public gb::msgpack::MsgPackBase
{
    int32_t  x{};
    std::string y;
    double   z{};

    TestPackerObj() = default;
    TestPackerObj(int32_t x_, std::string y_, double z_)
        : x(x_), y(std::move(y_)), z(z_) {}

    bool operator==(const TestPackerObj& o) const
    {
        return x == o.x && y == o.y && z == o.z;
    }

    REGISTER_PACKER(x, y, z)
};

// ============================================================
// Integer types
// ============================================================
TEST_CASE("msgpack: int8 序列化反序列化", "[msgpack][int]")
{
    REQUIRE(roundtrip(int8_t{0})     == int8_t{0});
    REQUIRE(roundtrip(int8_t{127})   == int8_t{127});
    REQUIRE(roundtrip(int8_t{-128})  == int8_t{-128});
    REQUIRE(roundtrip(int8_t{-1})    == int8_t{-1});
}

TEST_CASE("msgpack: int16 序列化反序列化", "[msgpack][int]")
{
    REQUIRE(roundtrip(int16_t{0})     == int16_t{0});
    REQUIRE(roundtrip(int16_t{32767}) == int16_t{32767});
    REQUIRE(roundtrip(int16_t{-32768})== int16_t{-32768});
    REQUIRE(roundtrip(int16_t{-1})    == int16_t{-1});
}

TEST_CASE("msgpack: int32 序列化反序列化", "[msgpack][int]")
{
    REQUIRE(roundtrip(int32_t{0})            == int32_t{0});
    REQUIRE(roundtrip(int32_t{2147483647})   == int32_t{2147483647});
    REQUIRE(roundtrip(int32_t{-2147483648})  == int32_t{-2147483648});
    REQUIRE(roundtrip(int32_t{-1})           == int32_t{-1});
    REQUIRE(roundtrip(int32_t{42})           == int32_t{42});
}

TEST_CASE("msgpack: int64 序列化反序列化", "[msgpack][int]")
{
    REQUIRE(roundtrip(int64_t{0})                      == int64_t{0});
    REQUIRE(roundtrip(int64_t{9223372036854775807LL})  == int64_t{9223372036854775807LL});
    REQUIRE(roundtrip(int64_t{-9223372036854775807LL - 1}) == int64_t{-9223372036854775807LL - 1});
    REQUIRE(roundtrip(int64_t{-1})                      == int64_t{-1});
}

TEST_CASE("msgpack: uint8 序列化反序列化", "[msgpack][uint]")
{
    REQUIRE(roundtrip(uint8_t{0})   == uint8_t{0});
    REQUIRE(roundtrip(uint8_t{255}) == uint8_t{255});
    REQUIRE(roundtrip(uint8_t{128}) == uint8_t{128});
}

TEST_CASE("msgpack: uint16 序列化反序列化", "[msgpack][uint]")
{
    REQUIRE(roundtrip(uint16_t{0})     == uint16_t{0});
    REQUIRE(roundtrip(uint16_t{65535}) == uint16_t{65535});
    REQUIRE(roundtrip(uint16_t{256})   == uint16_t{256});
}

TEST_CASE("msgpack: uint32 序列化反序列化", "[msgpack][uint]")
{
    REQUIRE(roundtrip(uint32_t{0})          == uint32_t{0});
    REQUIRE(roundtrip(uint32_t{4294967295}) == uint32_t{4294967295});
    REQUIRE(roundtrip(uint32_t{70000})      == uint32_t{70000});
}

TEST_CASE("msgpack: uint64 序列化反序列化", "[msgpack][uint]")
{
    REQUIRE(roundtrip(uint64_t{0})                    == uint64_t{0});
    REQUIRE(roundtrip(uint64_t{18446744073709551615ULL}) == uint64_t{18446744073709551615ULL});
    REQUIRE(roundtrip(uint64_t{100000})               == uint64_t{100000});
}

// ============================================================
// Floating point
// ============================================================
TEST_CASE("msgpack: float 序列化反序列化", "[msgpack][float]")
{
    // Non-integral values keep float format
    float v = 3.14159f;
    CHECK(roundtrip(v) == Catch::Approx(v).margin(0.001f));

    // Integral-valued floats pack as int → roundtrip back to exact float
    CHECK(roundtrip(42.0f) == 42.0f);
    CHECK(roundtrip(-7.0f) == -7.0f);
    CHECK(roundtrip(0.0f)  == 0.0f);
}

TEST_CASE("msgpack: double 序列化反序列化", "[msgpack][double]")
{
    double pi = 3.141592653589793;
    CHECK(roundtrip(pi) == Catch::Approx(pi).margin(1e-15));

    // Integral-valued doubles pack as int64 → roundtrip back
    CHECK(roundtrip(1.0e12) == 1.0e12);
    CHECK(roundtrip(-100.0) == -100.0);
}

// ============================================================
// Bool
// ============================================================
TEST_CASE("msgpack: bool 序列化反序列化", "[msgpack][bool]")
{
    REQUIRE(roundtrip(true)  == true);
    REQUIRE(roundtrip(false) == false);
}

// ============================================================
// String
// ============================================================
TEST_CASE("msgpack: string 序列化反序列化", "[msgpack][string]")
{
    REQUIRE(roundtrip(std::string{""})          == "");
    REQUIRE(roundtrip(std::string{"hello"})     == "hello");
    REQUIRE(roundtrip(std::string{"a"})         == "a");
    REQUIRE(roundtrip(std::string{" with spaces "}) == " with spaces ");

    // Longer string that exercises str8
    std::string long_str(200, 'x');
    REQUIRE(roundtrip(long_str) == long_str);
}

TEST_CASE("msgpack: const char* 打包为 string", "[msgpack][string]")
{
    // Packer::operator() accepts const Args& — string literals decay
    gb::msgpack::Packer pk;
    pk("literal_string", std::string("explicit"));
    auto data = pk.move();

    std::string s1, s2;
    gb::msgpack::Unpacker up(data.data(), data.size());
    up(s1, s2);
    REQUIRE(s1 == "literal_string");
    REQUIRE(s2 == "explicit");
    REQUIRE_FALSE(up.ec);
}

// ============================================================
// Enum (packs as int32)
// ============================================================
enum class TestColor : int32_t { Red = 0, Green = 1, Blue = 999 };

TEST_CASE("msgpack: 枚举序列化反序列化", "[msgpack][enum]")
{
    auto data = gb::msgpack::pack(TestColor::Green);
    auto [result] = gb::msgpack::unpack<TestColor>(data);
    REQUIRE(result == TestColor::Green);

    auto data2 = gb::msgpack::pack(TestColor::Blue);
    auto [result2] = gb::msgpack::unpack<TestColor>(data2);
    REQUIRE(result2 == TestColor::Blue);
}

// ============================================================
// nullptr / nil
// ============================================================
TEST_CASE("msgpack: nullptr 空值序列化反序列化", "[msgpack][nil]")
{
    auto data = gb::msgpack::pack(nullptr);
    // Unpack back as nullptr_t
    std::nullptr_t val = nullptr;
    gb::msgpack::Unpacker up(data.data(), data.size());
    up(val);
    REQUIRE(val == nullptr);
    REQUIRE_FALSE(up.ec);
}

// ============================================================
// Multiple values
// ============================================================
TEST_CASE("msgpack: 多值打包/解包", "[msgpack][multi]")
{
    auto data = gb::msgpack::pack(int32_t{42}, std::string{"hello"}, double{3.14});
    auto [i, s, d] = gb::msgpack::unpack<int32_t, std::string, double>(data);
    REQUIRE(i == 42);
    REQUIRE(s == "hello");
    REQUIRE(d == Catch::Approx(3.14).margin(1e-15));
}

TEST_CASE("msgpack: 通过 Packer/Unpacker 多值操作", "[msgpack][multi]")
{
    gb::msgpack::Packer pk;
    pk.process(int32_t{10}, int32_t{20}, int32_t{30});
    auto data = pk.vector();

    int32_t a{}, b{}, c{};
    gb::msgpack::Unpacker up(data.data(), data.size());
    up.process(a, b, c);
    REQUIRE(a == 10);
    REQUIRE(b == 20);
    REQUIRE(c == 30);
    REQUIRE_FALSE(up.ec);
}

// ============================================================
// std::array
// ============================================================
TEST_CASE("msgpack: std::array 序列化反序列化", "[msgpack][array]")
{
    std::array<int32_t, 4> arr = {1, 2, 3, 4};
    auto data = gb::msgpack::pack(arr);
    auto [result] = gb::msgpack::unpack<std::array<int32_t, 4>>(data);
    for (size_t i = 0; i < arr.size(); ++i)
        REQUIRE(result[i] == arr[i]);
}

// ============================================================
// Container: vector
// ============================================================
TEST_CASE("msgpack: vector<int> 序列化反序列化", "[msgpack][container][vector]")
{
    std::vector<int32_t> vec = {10, 20, 30, 40, 50};
    auto data = gb::msgpack::pack(vec);
    auto [result] = gb::msgpack::unpack<std::vector<int32_t>>(data);
    REQUIRE(result == vec);
}

TEST_CASE("msgpack: 空 vector 序列化反序列化", "[msgpack][container][vector]")
{
    std::vector<int32_t> empty;
    auto data = gb::msgpack::pack(empty);
    auto [result] = gb::msgpack::unpack<std::vector<int32_t>>(data);
    REQUIRE(result.empty());
}

TEST_CASE("msgpack: vector<string> 序列化反序列化", "[msgpack][container][vector]")
{
    std::vector<std::string> vec = {"alpha", "beta", "gamma"};
    auto data = gb::msgpack::pack(vec);
    auto [result] = gb::msgpack::unpack<std::vector<std::string>>(data);
    REQUIRE(result == vec);
}

TEST_CASE("msgpack: vector<float> 序列化反序列化", "[msgpack][container][vector]")
{
    std::vector<float> vec = {1.5f, 2.5f, -3.5f};
    auto data = gb::msgpack::pack(vec);
    auto [result] = gb::msgpack::unpack<std::vector<float>>(data);
    REQUIRE(result.size() == vec.size());
    for (size_t i = 0; i < vec.size(); ++i)
        CHECK(result[i] == Catch::Approx(vec[i]).margin(0.001f));
}

// ============================================================
// Container: list
// ============================================================
TEST_CASE("msgpack: list<int> 序列化反序列化", "[msgpack][container][list]")
{
    std::list<int32_t> lst = {1, 1, 2, 3, 5, 8};
    auto data = gb::msgpack::pack(lst);
    auto [result] = gb::msgpack::unpack<std::list<int32_t>>(data);
    REQUIRE(result == lst);
}

// ============================================================
// Container: map
// ============================================================
TEST_CASE("msgpack: map<string,int> 序列化反序列化", "[msgpack][container][map]")
{
    std::map<std::string, int32_t> mp;
    mp["a"] = 1;
    mp["b"] = 2;
    mp["c"] = 3;
    auto data = gb::msgpack::pack(mp);
    auto [result] = gb::msgpack::unpack<std::map<std::string, int32_t>>(data);
    REQUIRE(result == mp);
}

TEST_CASE("msgpack: 空 map 序列化反序列化", "[msgpack][container][map]")
{
    std::map<std::string, int32_t> empty;
    auto data = gb::msgpack::pack(empty);
    auto [result] = gb::msgpack::unpack<std::map<std::string, int32_t>>(data);
    REQUIRE(result.empty());
}

TEST_CASE("msgpack: unordered_map<int,int> 序列化反序列化", "[msgpack][container][map]")
{
    std::unordered_map<int32_t, int32_t> um;
    um[10] = 100;
    um[20] = 200;
    um[30] = 300;
    auto data = gb::msgpack::pack(um);
    auto [result] = gb::msgpack::unpack<std::unordered_map<int32_t, int32_t>>(data);
    REQUIRE(result == um);
}

// ============================================================
// Binary (vector<uint8_t>)
// ============================================================
TEST_CASE("msgpack: vector<uint8_t> 二进制序列化反序列化", "[msgpack][binary]")
{
    std::vector<uint8_t> bin = {0x00, 0x01, 0xFF, 0xAB, 0xCD};
    auto data = gb::msgpack::pack(bin);
    auto [result] = gb::msgpack::unpack<std::vector<uint8_t>>(data);
    REQUIRE(result == bin);
}

TEST_CASE("msgpack: 空二进制序列化反序列化", "[msgpack][binary]")
{
    std::vector<uint8_t> bin;
    auto data = gb::msgpack::pack(bin);
    auto [result] = gb::msgpack::unpack<std::vector<uint8_t>>(data);
    REQUIRE(result.empty());
}

// ============================================================
// std::chrono::time_point
// ============================================================
TEST_CASE("msgpack: 时间点序列化反序列化", "[msgpack][timepoint]")
{
    auto now = std::chrono::system_clock::now();
    auto data = gb::msgpack::pack(now);
    auto [result] = gb::msgpack::unpack<std::chrono::system_clock::time_point>(data);
    REQUIRE(result == now);
}

// ============================================================
// Custom MsgPackBase object with REGISTER_PACKER
// ============================================================
TEST_CASE("msgpack: REGISTER_PACKER 自定义对象序列化反序列化", "[msgpack][custom]")
{
    TestPackerObj in{42, "hello", 3.14};
    auto data = gb::msgpack::pack(in);
    auto [out] = gb::msgpack::unpack<TestPackerObj>(data);
    REQUIRE(out == in);
}

TEST_CASE("msgpack: REGISTER_PACKER 默认值对象序列化反序列化", "[msgpack][custom]")
{
    TestPackerObj in;
    auto data = gb::msgpack::pack(in);
    auto [out] = gb::msgpack::unpack<TestPackerObj>(data);
    REQUIRE(out == in);
}

// ============================================================
// Packer direct API: vector, move, clear, empty
// ============================================================
TEST_CASE("msgpack: Packer vector/move/clear 接口", "[msgpack][packer-api]")
{
    gb::msgpack::Packer pk;
    REQUIRE(pk.vector().empty());

    pk(42);
    REQUIRE_FALSE(pk.vector().empty());

    // move() empties internal vector via move semantics
    auto v1 = pk.move();
    REQUIRE_FALSE(v1.empty());
    REQUIRE(pk.vector().empty()); // after move, vector is now empty

    pk(99);
    pk.clear();
    REQUIRE(pk.vector().empty());
}

// ============================================================
// Unpacker direct API: empty, set_data, ec
// ============================================================
TEST_CASE("msgpack: Unpacker empty/set_data 接口", "[msgpack][unpacker-api]")
{
    // Default-constructed unpacker is empty (dataPointer == dataEnd)
    gb::msgpack::Unpacker empty_up;
    REQUIRE(empty_up.empty());

    // Pack something, then feed it via set_data
    auto data = gb::msgpack::pack(int32_t{77});
    gb::msgpack::Unpacker up;
    REQUIRE(up.empty());
    up.set_data(data.data(), data.size());
    REQUIRE_FALSE(up.empty());

    int32_t val = 0;
    up(val);
    REQUIRE(val == 77);
    REQUIRE_FALSE(up.ec);
}

// ============================================================
// Free function pack/unpack error code overloads
// ============================================================
TEST_CASE("msgpack: 自由函数 unpack 带错误码", "[msgpack][errors]")
{
    auto data = gb::msgpack::pack(int32_t{100});
    std::error_code ec;
    auto [val] = gb::msgpack::unpack<int32_t>(data.data(), data.size(), ec);
    REQUIRE_FALSE(ec);
    REQUIRE(val == 100);
}

TEST_CASE("msgpack: 解包空数据产生错误", "[msgpack][errors]")
{
    // Unpacking from an empty buffer should set error_code
    uint8_t empty_buf[1] = {0};
    std::error_code ec;
    auto result = gb::msgpack::unpack<int32_t>(empty_buf, (size_t)0, ec);
    CHECK(ec.value() == static_cast<int>(gb::msgpack::UnPackErrorType::eOutRange));
    (void)result;
}

TEST_CASE("msgpack: 解包截断数据产生错误", "[msgpack][errors]")
{
    // Pack a 4-byte value, then only supply first byte when unpacking
    auto full = gb::msgpack::pack(uint32_t{100000});
    REQUIRE(full.size() > 1);

    std::error_code ec;
    // Only give 1 byte of the full message
    auto result = gb::msgpack::unpack<uint32_t>(full.data(), (size_t)1, ec);
    CHECK(ec.value() == static_cast<int>(gb::msgpack::UnPackErrorType::eOutRange));
    (void)result;
}

// ============================================================
// Variable-size encoding verification
// ============================================================
TEST_CASE("msgpack: 小整数使用更紧凑的编码格式", "[msgpack][encoding]")
{
    // int32_t 0 → packs as int8 (2 bytes: 0xd0 + 0x00)
    auto small = gb::msgpack::pack(int32_t{0});
    CHECK(small.size() == 2);

    // int32_t 20000 → larger than int8, uses int16 (3 bytes: 0xd1 + 2 bytes)
    auto medium = gb::msgpack::pack(int32_t{20000});
    CHECK(medium.size() == 3);

    // int32_t 100000 → larger than int16, uses int32 (5 bytes: 0xd2 + 4 bytes)
    auto large = gb::msgpack::pack(int32_t{100000});
    CHECK(large.size() == 5);

    // uint8_t always uses 2 bytes (0xcc + value)
    auto u8 = gb::msgpack::pack(uint8_t{200});
    CHECK(u8.size() == 2);

    // uint16_t beyond 255 uses 3 bytes
    auto u16 = gb::msgpack::pack(uint16_t{1000});
    CHECK(u16.size() == 3);
}

TEST_CASE("msgpack: 整数值的 float/double 编码为整数类型", "[msgpack][encoding]")
{
    // float 42.0 → packs as int8 (downcast chain: int32 → int16 → int8)
    auto f = gb::msgpack::pack(42.0f);
    CHECK(f[0] == (uint8_t)gb::msgpack::format_t::int8);

    // double 3.0 → packs as int8
    auto d = gb::msgpack::pack(3.0);
    CHECK(d[0] == (uint8_t)gb::msgpack::format_t::int8);

    // double 1e12 → larger than int32 range → packs as int64
    auto big = gb::msgpack::pack(1.0e12);
    CHECK(big[0] == (uint8_t)gb::msgpack::format_t::int64);
}

// ============================================================
// Mixed-type tuple edge cases
// ============================================================
TEST_CASE("msgpack: 单值 tuple 序列化反序列化", "[msgpack][tuple]")
{
    // Use std::tuple directly
    std::tuple<int32_t> t{42};
    auto data = gb::msgpack::pack(t);
    auto [val] = gb::msgpack::unpack<int32_t>(data);
    REQUIRE(val == 42);
}

TEST_CASE("msgpack: 异构 tuple 自由函数序列化反序列化", "[msgpack][tuple]")
{
    auto data = gb::msgpack::pack(
        int64_t{-1},
        uint32_t{100},
        std::string{"world"},
        double{3.14}
    );
    auto [a, b, c, d] = gb::msgpack::unpack<int64_t, uint32_t, std::string, double>(data);
    REQUIRE(a == -1);
    REQUIRE(b == 100);
    REQUIRE(c == "world");
    REQUIRE(d == Catch::Approx(3.14).margin(1e-15));
}
