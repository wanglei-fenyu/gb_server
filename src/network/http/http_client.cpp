#include "http_client.h"
#include "log/log.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/system/system_error.hpp>

#include <regex>

namespace asio     = boost::asio;
namespace beast    = boost::beast;
namespace beast_http = boost::beast::http;
using    tcp       = asio::ip::tcp;


namespace gb
{

// ---------------------------------------------------------------------------
// URL 解析
// ---------------------------------------------------------------------------
HttpClient::ParsedUrl HttpClient::ParseUrl(const std::string& url)
{
    ParsedUrl result;
    result.target = "/";

    // 正则：(https?://)?(host)(:port)?(/path)?
    static const std::regex re(
        R"(^(https?)://([^:/?#]+)(?::(\d+))?([^#?]*))",
        std::regex::icase);

    std::smatch m;
    if (std::regex_match(url, m, re))
    {
        result.is_ssl = (m[1].str() == "https");
        result.host   = m[2].str();
        result.port   = m[3].str();
        result.target = m[4].str();
        if (result.target.empty())
            result.target = "/";

        if (result.port.empty())
            result.port = result.is_ssl ? "443" : "80";
    }
    else
    {
        // 视为原始 host:port
        result.host = url;
        result.port = "80";
    }
    return result;
}

// ---------------------------------------------------------------------------
// HttpClient 实现
// ---------------------------------------------------------------------------
HttpClient::HttpClient(asio::io_context& ioc) :
    ioc_(ioc)
{
}

HttpClient::~HttpClient() = default;

// ---- 协程 API -------------------------------------------------------------

asio::awaitable<HttpResponse> HttpClient::Get(const std::string& url)
{
    co_return co_await DoRequest(beast_http::verb::get, url, "", "");
}

asio::awaitable<HttpResponse> HttpClient::Post(
    const std::string& url,
    const std::string& body,
    const std::string& content_type)
{
    co_return co_await DoRequest(beast_http::verb::post, url, body, content_type);
}

asio::awaitable<HttpResponse> HttpClient::Put(
    const std::string& url,
    const std::string& body,
    const std::string& content_type)
{
    co_return co_await DoRequest(beast_http::verb::put, url, body, content_type);
}

asio::awaitable<HttpResponse> HttpClient::Delete(const std::string& url)
{
    co_return co_await DoRequest(beast_http::verb::delete_, url, "", "");
}

// ---- 回调 API -------------------------------------------------------------

void HttpClient::Get(const std::string& url, Callback cb)
{
    auto self = shared_from_this();
    asio::co_spawn(ioc_, [self, url, cb]() -> asio::awaitable<void> {
            try {
                auto res = co_await self->Get(url);
                if (cb) cb(std::move(res));
            } catch (const std::exception& e) {
                if (cb) cb(HttpResponse{-1, e.what(), "text/plain"});
            } }, asio::detached);
}

void HttpClient::Post(const std::string& url, const std::string& body, Callback cb, const std::string& content_type)
{
    auto self = shared_from_this();
    asio::co_spawn(ioc_, [self, url, body, cb, content_type]() -> asio::awaitable<void> {
            try {
                auto res = co_await self->Post(url, body, content_type);
                if (cb) cb(std::move(res));
            } catch (const std::exception& e) {
                if (cb) cb(HttpResponse{-1, e.what(), "text/plain"});
            } }, asio::detached);
}

void HttpClient::Put(const std::string& url, const std::string& body, Callback cb, const std::string& content_type)
{
    auto self = shared_from_this();
    asio::co_spawn(ioc_, [self, url, body, cb, content_type]() -> asio::awaitable<void> {
            try {
                auto res = co_await self->Put(url, body, content_type);
                if (cb) cb(std::move(res));
            } catch (const std::exception& e) {
                if (cb) cb(HttpResponse{-1, e.what(), "text/plain"});
            } }, asio::detached);
}

void HttpClient::Delete(const std::string& url, Callback cb)
{
    auto self = shared_from_this();
    asio::co_spawn(ioc_, [self, url, cb]() -> asio::awaitable<void> {
            try {
                auto res = co_await self->Delete(url);
                if (cb) cb(std::move(res));
            } catch (const std::exception& e) {
                if (cb) cb(HttpResponse{-1, e.what(), "text/plain"});
            } }, asio::detached);
}

// ---- 内部实现 -------------------------------------------------------------

asio::awaitable<HttpResponse> HttpClient::DoRequest(
    beast_http::verb   method,
    const std::string& url,
    const std::string& body,
    const std::string& content_type)
{
    auto parsed = ParseUrl(url);

    HttpResponse result;
    result.content_type = "text/plain";

    try
    {
        // DNS 解析
        tcp::resolver resolver(ioc_);
        auto          endpoints = co_await resolver.async_resolve(
            parsed.host, parsed.port, asio::use_awaitable);

        if (parsed.is_ssl)
        {
            // ---- HTTPS 请求 ----
            asio::ssl::context ssl_ctx(asio::ssl::context::tlsv12_client);
            ssl_ctx.set_default_verify_paths();
            ssl_ctx.set_verify_mode(asio::ssl::verify_peer | asio::ssl::verify_fail_if_no_peer_cert);

            beast::ssl_stream<beast::tcp_stream> stream(ioc_, ssl_ctx);

            // 设置超时
            stream.next_layer().expires_after(
                std::chrono::seconds(timeouts_.connect_timeout_seconds));

            // 连接
            co_await stream.next_layer().async_connect(endpoints, asio::use_awaitable);

            // SSL 握手
            stream.next_layer().expires_after(
                std::chrono::seconds(timeouts_.read_timeout_seconds));
            co_await stream.async_handshake(asio::ssl::stream_base::client, asio::use_awaitable);

            // 构建请求
            beast_http::request<beast_http::string_body> req{method, parsed.target, 11};
            req.set(beast_http::field::host, parsed.host);
            req.set(beast_http::field::user_agent, "gb_server/1.0");

            if (method == beast_http::verb::post || method == beast_http::verb::put)
            {
                req.set(beast_http::field::content_type, content_type);
                req.body() = body;
                req.prepare_payload();
            }

            // 设置写超时
            stream.next_layer().expires_after(
                std::chrono::seconds(timeouts_.write_timeout_seconds));

            // 发送请求
            co_await beast_http::async_write(stream, req, asio::use_awaitable);

            // 读取响应
            beast::flat_buffer                            buffer;
            beast_http::response<beast_http::string_body> res;
            stream.next_layer().expires_after(
                std::chrono::seconds(timeouts_.read_timeout_seconds));
            co_await beast_http::async_read(stream, buffer, res, asio::use_awaitable);

            result.status       = static_cast<int>(res.result_int());
            result.body         = res.body();
            result.content_type = std::string(res[beast_http::field::content_type]);

            // 优雅关闭（忽略SSL关闭错误）
            beast::error_code ec;
            stream.async_shutdown([](beast::error_code) { /* ignore */ });
        }
        else
        {
            // ---- HTTP 请求 ----
            beast::tcp_stream stream(ioc_);

            // Connect
            stream.expires_after(
                std::chrono::seconds(timeouts_.connect_timeout_seconds));
            co_await stream.async_connect(endpoints, asio::use_awaitable);

            // 构建请求
            beast_http::request<beast_http::string_body> req{method, parsed.target, 11};
            req.set(beast_http::field::host, parsed.host);
            req.set(beast_http::field::user_agent, "gb_server/1.0");

            if (method == beast_http::verb::post || method == beast_http::verb::put)
            {
                req.set(beast_http::field::content_type, content_type);
                req.body() = body;
                req.prepare_payload();
            }

            // 发送
            stream.expires_after(
                std::chrono::seconds(timeouts_.write_timeout_seconds));
            co_await beast_http::async_write(stream, req, asio::use_awaitable);

            // 读取响应
            beast::flat_buffer                            buffer;
            beast_http::response<beast_http::string_body> res;
            stream.expires_after(
                std::chrono::seconds(timeouts_.read_timeout_seconds));
            co_await beast_http::async_read(stream, buffer, res, asio::use_awaitable);

            result.status       = static_cast<int>(res.result_int());
            result.body         = res.body();
            result.content_type = std::string(res[beast_http::field::content_type]);

            // 关闭
            beast::error_code ec;
            stream.socket().shutdown(tcp::socket::shutdown_both, ec);
        }
    }
    catch (const beast::system_error& e)
    {
        LOG_ERROR("HttpClient error [{} {}]: {}",
                  std::string(beast_http::to_string(method)), url, e.what());
        result.status = -1;
        result.body   = e.what();
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("HttpClient error [{} {}]: {}",
                  std::string(beast_http::to_string(method)), url, e.what());
        result.status = -1;
        result.body   = e.what();
    }

    co_return result;
}
} // namespace gb
