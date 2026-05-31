#include "http_server_internal.h"
#include "log/log.h"

#include <boost/beast/core/bind_handler.hpp>

// ---------------------------------------------------------------------------
// Impl 实现
// ---------------------------------------------------------------------------

namespace gb
{

HttpServer::Impl::Impl()
    : ioc_(std::make_unique<asio::io_context>(1))
{
}

HttpServer::Impl::~Impl()
{
    Stop();
}

bool HttpServer::Impl::Start(const std::string& ip, uint16_t port, int thread_count,
                             const std::string* cert_file, const std::string* key_file)
{
    if (running_.load())
    {
        LOG_WARN("HttpServer already running");
        return false;
    }

    // 如果提供了证书/密钥则创建SSL上下文
    if (cert_file && key_file)
    {
        ssl_ctx_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tlsv12_server);
        ssl_ctx_->set_options(
            asio::ssl::context::default_workarounds |
            asio::ssl::context::no_sslv2 |
            asio::ssl::context::no_sslv3 |
            asio::ssl::context::single_dh_use);

        ssl_ctx_->use_certificate_file(*cert_file, asio::ssl::context::pem);
        ssl_ctx_->use_private_key_file(*key_file, asio::ssl::context::pem);
    }

    // 创建监听器
    tcp::endpoint ep(asio::ip::make_address(ip), port);
    listener_ = std::make_shared<Listener>(*this, *ioc_, ep);

    // Work guard 保持 io_context 运行
    work_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
        ioc_->get_executor());

    // 启动监听器
    listener_->Run();

    // 启动IO线程
    for (int i = 0; i < thread_count; ++i)
    {
        threads_.emplace_back([this]() {
            ioc_->run();
        });
    }

    running_.store(true);
    LOG_INFO("HttpServer started on {}:{} ({} threads, {})",
             ip, port, thread_count, ssl_ctx_ ? "HTTPS" : "HTTP");
    return true;
}

void HttpServer::Impl::Stop()
{
    if (!running_.exchange(false))
        return;

    if (listener_)
        listener_->Stop();

    work_.reset();
    ioc_->stop();

    for (auto& t : threads_)
    {
        if (t.joinable())
            t.join();
    }
    threads_.clear();

    ioc_ = std::make_unique<asio::io_context>(1);
    LOG_INFO("HttpServer stopped");
}

void HttpServer::Impl::AddRoute(const std::string& path, beast_http::verb method,
                                HttpRequestHandler handler)
{
    std::lock_guard<std::mutex> lock(routes_mutex_);
    routes_.push_back({path, method, std::move(handler)});
}

void HttpServer::Impl::Dispatch(const std::string& path, beast_http::verb method,
                                const HttpRequest& req, HttpResponse& res)
{
    std::vector<RouteEntry> routes_snapshot;
    {
        std::lock_guard<std::mutex> lock(routes_mutex_);
        routes_snapshot = routes_;
    }

    for (auto& entry : routes_snapshot)
    {
        if (entry.method == method && MatchRoute(entry.path, path))
        {
            try
            {
                entry.handler(req, res);
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("Http handler error [{} {}]: {}",
                          std::string(beast_http::to_string(method)), path, e.what());
                res.status      = 500;
                res.body        = R"({"code":500,"msg":"internal server error"})";
                res.content_type = "application/json";
            }
            return;
        }
    }

    res.status      = 404;
    res.body        = R"({"code":404,"msg":"not found"})";
    res.content_type = "application/json";
}

// ---------------------------------------------------------------------------
// Listener 实现
// ---------------------------------------------------------------------------

HttpServer::Impl::Listener::Listener(Impl& owner, asio::io_context& ioc,
                                     const tcp::endpoint& ep)
    : owner_(owner), acceptor_(ioc), ioc_(ioc)
{
    beast::error_code ec;

    acceptor_.open(ep.protocol(), ec);
    if (ec) { LOG_ERROR("Listener open: {}", ec.message()); return; }

    acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) { LOG_ERROR("Listener set_option: {}", ec.message()); return; }

    acceptor_.bind(ep, ec);
    if (ec) { LOG_ERROR("Listener bind: {}", ec.message()); return; }

    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) { LOG_ERROR("Listener listen: {}", ec.message()); return; }
}

void HttpServer::Impl::Listener::Run()
{
    DoAccept();
}

void HttpServer::Impl::Listener::Stop()
{
    beast::error_code ec;
    acceptor_.close(ec);
}

void HttpServer::Impl::Listener::DoAccept()
{
    acceptor_.async_accept(ioc_,
        beast::bind_front_handler(&Listener::OnAccept, shared_from_this()));
}

void HttpServer::Impl::Listener::OnAccept(beast::error_code ec, tcp::socket socket)
{
    if (ec)
    {
        LOG_ERROR("Listener accept: {}", ec.message());
        return;
    }

    if (owner_.ssl_ctx_)
    {
        auto session = std::make_shared<HttpsSession>(
            owner_, beast::tcp_stream(std::move(socket)), *owner_.ssl_ctx_);
        session->Run();
    }
    else
    {
        auto session = std::make_shared<HttpSession>(
            owner_, beast::tcp_stream(std::move(socket)));
        session->Run();
    }

    DoAccept();
}

} // namespace gb

// ---------------------------------------------------------------------------
// HttpServer 公开 API
// ---------------------------------------------------------------------------

namespace gb
{

HttpServer::HttpServer()
    : impl_(std::make_unique<Impl>())
{
}

HttpServer::~HttpServer()
{
    Stop();
}

bool HttpServer::Start(const std::string& ip, uint16_t port, int thread_count)
{
    return impl_->Start(ip, port, thread_count, nullptr, nullptr);
}

bool HttpServer::StartSSL(const std::string& ip, uint16_t port,
                          const std::string& cert_file,
                          const std::string& key_file,
                          int thread_count)
{
    return impl_->Start(ip, port, thread_count, &cert_file, &key_file);
}

void HttpServer::Stop()
{
    impl_->Stop();
}

bool HttpServer::IsRunning() const
{
    return impl_->running_.load();
}

void HttpServer::Get(const std::string& path, HttpRequestHandler handler)
{
    AddRoute(path, beast_http::verb::get, std::move(handler));
}

void HttpServer::Post(const std::string& path, HttpRequestHandler handler)
{
    AddRoute(path, beast_http::verb::post, std::move(handler));
}

void HttpServer::AddRoute(const std::string& path,
                          beast_http::verb    method,
                          HttpRequestHandler  handler)
{
    impl_->AddRoute(path, method, std::move(handler));
}

} // namespace gb
