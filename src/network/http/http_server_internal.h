#pragma once
#include "http_server.h"
#include "http_session.h"
#include "https_session.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/http.hpp>

#include <thread>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>
#include <map>
#include <sstream>

namespace asio     = boost::asio;
namespace beast    = boost::beast;
namespace beast_http = boost::beast::http;
using    tcp       = asio::ip::tcp;

namespace gb
{

// ---------------------------------------------------------------------------
// 辅助函数（内联，可在头文件中使用）
// ---------------------------------------------------------------------------

inline std::map<std::string, std::string> ParseQueryString(const std::string& target)
{
    std::map<std::string, std::string> params;
    auto pos = target.find('?');
    if (pos == std::string::npos)
        return params;

    std::string query = target.substr(pos + 1);
    std::stringstream ss(query);
    std::string       pair;
    while (std::getline(ss, pair, '&'))
    {
        auto eq = pair.find('=');
        if (eq != std::string::npos)
            params[pair.substr(0, eq)] = pair.substr(eq + 1);
        else
            params[pair] = "";
    }
    return params;
}

inline std::string GetPath(const std::string& target)
{
    auto pos = target.find('?');
    return pos == std::string::npos ? target : target.substr(0, pos);
}

inline bool MatchRoute(const std::string& pattern, const std::string& path)
{
    return pattern == path;
}

template <class Body>
beast_http::response<beast_http::string_body>
    ToBeastResponse(const HttpResponse& res, const beast_http::request<Body>& req)
{
    beast_http::response<beast_http::string_body> beast_res(
        static_cast<beast_http::status>(res.status), req.version());
    beast_res.set(beast_http::field::content_type, res.content_type);
    beast_res.set(beast_http::field::server, "gb_server");
    beast_res.keep_alive(req.keep_alive());
    for (auto& [k, v] : res.headers)
        beast_res.set(k, v);
    beast_res.body() = res.body;
    beast_res.prepare_payload();
    return beast_res;
}

inline HttpRequest ToHttpRequest(const beast_http::request<beast_http::string_body>& beast_req)
{
    HttpRequest req;
    req.method       = beast_req.method();
    req.target       = std::string(beast_req.target());
    req.body         = beast_req.body();
    req.content_type = std::string(beast_req[beast_http::field::content_type]);
    req.params       = ParseQueryString(req.target);

    for (auto const& f : beast_req)
        req.headers[std::string(f.name_string())] = std::string(f.value());

    return req;
}

// ---------------------------------------------------------------------------
// HttpServer 内部实现
// ---------------------------------------------------------------------------
struct HttpServer::Impl
{
    struct RouteEntry
    {
        std::string        path;
        beast_http::verb   method;
        HttpRequestHandler handler;
    };

    // -- TCP 监听器 ---------------------------------------------------------
    class Listener : public std::enable_shared_from_this<Listener>
    {
    public:
        Listener(Impl& owner, asio::io_context& ioc, const tcp::endpoint& ep);

        void Run();
        void Stop();

    private:
        void DoAccept();
        void OnAccept(beast::error_code ec, tcp::socket socket);

        Impl&             owner_;
        tcp::acceptor     acceptor_;
        asio::io_context& ioc_;
    };

    // -- Impl 成员 ----------------------------------------------------------
    std::vector<std::thread>            threads_;
    std::unique_ptr<asio::io_context>   ioc_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::shared_ptr<Listener>           listener_;
    std::unique_ptr<asio::ssl::context> ssl_ctx_;

    std::vector<RouteEntry> routes_;
    std::mutex              routes_mutex_;
    size_t                  max_body_size_ = 1024 * 1024;
    std::atomic<bool>       running_{false};

    // -- Impl 方法 ----------------------------------------------------------
    Impl();
    ~Impl();

    bool Start(const std::string& ip, uint16_t port, int thread_count,
               const std::string* cert_file, const std::string* key_file);
    void Stop();
    void AddRoute(const std::string& path, beast_http::verb method, HttpRequestHandler handler);
    void Dispatch(const std::string& path, beast_http::verb method,
                  const HttpRequest& req, HttpResponse& res);
};

} // namespace gb
