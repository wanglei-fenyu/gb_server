/// Boost 异常桩函数。
/// libboost_exception.lib 在 MSVC 下是空存根，不提供 throw_exception 符号。
/// 此处提供本地实现以解决链接错误。
#include <boost/throw_exception.hpp>
#include <exception>

namespace boost {

BOOST_NORETURN void throw_exception(std::exception const& e, boost::source_location const& loc)
{
    std::terminate();
}

BOOST_NORETURN void throw_exception(std::exception const& e)
{
    std::terminate();
}

} // namespace boost
