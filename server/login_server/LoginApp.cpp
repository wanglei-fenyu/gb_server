#include "LoginApp.h"
#include "app/types.h"
#include "base/res_path.h"

// HTTP 连接 io_context 池大小（每个 io_context 一个线程，连接轮询分布）
static constexpr size_t HTTP_IO_THREADS = 4;

LoginApp::LoginApp(int argc, char* argv[])
    : ServerApp(argc, argv)
{
}

int LoginApp::OnServerInit()
{
    LOG_INFO("LoginServer::OnServerInit");

    // ---- 注册 TCP 处理器（服务器间通信，预留 NATS）----
    // TODO: 注册服务器间通信的 TCP 处理器

    // ---- 启动 HTTP 服务器（面向客户端）----
    auto [http_ip, http_port_str] = AppTypeMgr::Instance()->GetServerIpPort(
        APP_LOGIN,
#ifdef WIN32
        UIR_TYPE::UT_WIN_HTTP
#else
        UIR_TYPE::UT_LINUX_HTTP
#endif
    );
    uint16_t http_port = static_cast<uint16_t>(std::stoul(http_port_str));

    // 注册登录路由
    http_server_.config.address = http_ip;
    http_server_.config.port = http_port;
    http_server_.resource["^/api/login$"]["POST"] = [](std::shared_ptr<HttpServer::Response> res, std::shared_ptr<HttpServer::Request> req) {
        LOG_INFO("Login request: {}", req->content.string());
        // TODO: 实际的登录鉴权逻辑
        res->write(R"({"code":0,"msg":"ok","data":{}})", {{"Content-Type", "application/json"}});
        res->send();
    };

    // HTTP 线程模型与 TCP 服务器统一（外部 IoServicePool 形式）：
    // acceptor 挂在专用维护线程的 io_context 上，连接经 io_context_selector 轮询分布到池中。
    // 因此 accept_and_run() 仅 listen + accept 即返回，不阻塞、不启动内部线程。
    http_io_pool_ = std::make_shared<gb::IoServicePool>(HTTP_IO_THREADS);
    http_io_pool_->Run();

    http_server_.external_io_context = &http_io_pool_->GetIoService().second;
    http_server_.io_context_selector = [this]() -> gb::IoService& {
        return http_io_pool_->GetIoService().second;
    };

    try
    {
        http_server_.bind();
        http_server_.accept_and_run();
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("LoginServer failed to start HTTP server on {}:{}: {}", http_ip, http_port, e.what());
        http_server_.stop();
        if (http_io_pool_)
            http_io_pool_->GracefulStop();
        http_io_pool_.reset();
        return -1;
    }

    LOG_INFO("LoginServer HTTP listening on {}:{}", http_ip, http_port);
    return 0;
}

int LoginApp::OnCleanup()
{
    http_server_.stop();
    if (http_io_pool_)
        http_io_pool_->GracefulStop();
    http_io_pool_.reset();
    return ServerApp::OnCleanup();
}
