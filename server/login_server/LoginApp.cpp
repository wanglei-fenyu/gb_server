#include "LoginApp.h"
#include "app/types.h"
#include "base/res_path.h"

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

    try
    {
        http_server_.bind(); // 在本线程绑定端口，绑定失败会在此处抛出异常
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("LoginServer failed to start HTTP server on {}:{}: {}", http_ip, http_port, e.what());
        return -1;
    }
    http_server_thread_ = std::thread([this]() { http_server_.accept_and_run(); });

    LOG_INFO("LoginServer HTTP listening on {}:{}", http_ip, http_port);
    return 0;
}

int LoginApp::OnCleanup()
{
    http_server_.stop();
    if (http_server_thread_.joinable())
        http_server_thread_.join();
    return ServerApp::OnCleanup();
}
