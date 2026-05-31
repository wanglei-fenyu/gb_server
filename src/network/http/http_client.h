#pragma once
#include "http_common.h"
#include <memory>
#include <string>
#include <boost/asio/io_context.hpp>
#include <boost/asio/awaitable.hpp>

namespace gb
{
/// 基于 Boost.Beast 的 HTTP/HTTPS 客户端，支持 SSL。
///
/// 支持回调和 C++20 协程（asio::awaitable）。
///
/// 用法（协程）：
///   HttpClient client(ioc);
///   auto res = co_await client.Get("https://example.com/api");
///
/// 用法（回调）：
///   HttpClient client(ioc);
///   client.Get("http://example.com/api", [](HttpResponse res) {
///       LOG_INFO("status={} body={}", res.status, res.body);
///   });
class HttpClient : public std::enable_shared_from_this<HttpClient>
{
public:
    /// 每个请求的超时设置。
    struct Timeouts
    {
        int connect_timeout_seconds = 10;
        int read_timeout_seconds    = 30;
        int write_timeout_seconds   = 30;
    };

    explicit HttpClient(boost::asio::io_context& ioc);
    ~HttpClient();

    HttpClient(const HttpClient&)            = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // ---- 协程 API (asio::awaitable) ----------------------------------------

    /// 执行GET请求。
    boost::asio::awaitable<HttpResponse> Get(const std::string& url);

    /// 执行POST请求，带请求体。
    boost::asio::awaitable<HttpResponse> Post(const std::string& url,
                                              const std::string& body,
                                              const std::string& content_type = "application/json");

    /// 执行PUT请求。
    boost::asio::awaitable<HttpResponse> Put(const std::string& url,
                                             const std::string& body,
                                             const std::string& content_type = "application/json");

    /// 执行DELETE请求。
    boost::asio::awaitable<HttpResponse> Delete(const std::string& url);

    // ---- 回调 API ----------------------------------------------------------

    using Callback = std::function<void(HttpResponse)>;

    void Get(const std::string& url, Callback cb);
    void Post(const std::string& url, const std::string& body, Callback cb, const std::string& content_type = "application/json");
    void Put(const std::string& url, const std::string& body, Callback cb, const std::string& content_type = "application/json");
    void Delete(const std::string& url, Callback cb);

    // ---- 配置 --------------------------------------------------------------

    void            SetTimeouts(const Timeouts& t) { timeouts_ = t; }
    const Timeouts& GetTimeouts() const { return timeouts_; }

private:
    struct ParsedUrl
    {
        std::string host;
        std::string port;
        std::string target;
        bool        is_ssl = false;
    };

    static ParsedUrl ParseUrl(const std::string& url);

    boost::asio::awaitable<HttpResponse> DoRequest(
        boost::beast::http::verb method,
        const std::string&       url,
        const std::string&       body,
        const std::string&       content_type);

    boost::asio::io_context& ioc_;
    Timeouts                 timeouts_;
};

} // namespace gb
