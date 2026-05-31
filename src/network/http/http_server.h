#pragma once
#include "http_common.h"
#include <memory>
#include <string>
#include <boost/beast/http/verb.hpp>
namespace gb {

/// 基于 Boost.Beast 的 HTTP/HTTPS 服务器。
///
/// 支持基于路径的路由注册（Get/Post/AddRoute）。
/// Start() 或 StartSSL() 后，服务器在内部线程池上运行，
/// 并在其 IO 线程中调用处理器。
///
/// 用法：
///   HttpServer srv;
///   srv.Post("/api/login", [](const HttpRequest& req, HttpResponse& res) {
///       res.SetJsonBody(R"({"code":0})");
///   });
///   srv.Start("0.0.0.0", 8080);
class HttpServer
{
public:
    struct Impl;

    HttpServer();
    ~HttpServer();

    // 不可拷贝
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /// 启动HTTP服务器。
    /// @param ip          监听地址（如"0.0.0.0"、"127.0.0.1"）
    /// @param port        监听端口
    /// @param thread_count  IO线程数（默认2）
    /// @return 成功返回true
    bool Start(const std::string& ip, uint16_t port, int thread_count = 2);

    /// 启动HTTPS服务器（SSL/TLS）。
    /// @param ip           监听地址
    /// @param port         监听端口
    /// @param cert_file    PEM证书文件路径
    /// @param key_file     PEM私钥文件路径
    /// @param thread_count  IO线程数（默认2）
    /// @return 成功返回true
    bool StartSSL(const std::string& ip, uint16_t port,
                  const std::string& cert_file,
                  const std::string& key_file,
                  int thread_count = 2);

    /// 优雅停止服务器（等待活跃会话结束）。
    void Stop();

    bool IsRunning() const;

    // ---- 路由注册 ----------------------------------------------------------

    void Get(const std::string& path, HttpRequestHandler handler);
    void Post(const std::string& path, HttpRequestHandler handler);
    void AddRoute(const std::string&             path,
                  boost::beast::http::verb       method,
                  HttpRequestHandler             handler);

private:
    std::unique_ptr<Impl> impl_;
};

}
