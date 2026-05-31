#pragma once
#include "http_common.h"
#include "http_server.h"
#include <memory>
#include <optional>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

namespace asio     = boost::asio;
namespace beast    = boost::beast;
namespace beast_http = boost::beast::http;

namespace gb
{
struct HttpServer::Impl;

/// HTTP会话（非SSL）。
class HttpSession : public std::enable_shared_from_this<HttpSession>
{
public:
    HttpSession(HttpServer::Impl& owner, beast::tcp_stream&& stream);

    void Run();

private:
    void DoRead();
    void OnRead(beast::error_code ec, size_t bytes);
    void DoClose();

    HttpServer::Impl&                             owner_;
    beast::tcp_stream                             stream_;
    beast::flat_buffer                            buffer_;
    std::optional<beast_http::request_parser<beast_http::string_body>> parser_;
};

} // namespace gb
