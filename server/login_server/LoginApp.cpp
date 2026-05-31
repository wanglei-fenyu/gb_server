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

    // ---- Register TCP handlers (inter-server communication, future NATS) ----
    // TODO: register TCP handlers for internal server communication

    // ---- Start HTTP server (client-facing) ----
    auto [http_ip, http_port_str] = AppTypeMgr::Instance()->GetServerIpPort(
        APP_LOGIN,
#ifdef WIN32
        UIR_TYPE::UT_WIN_HTTP
#else
        UIR_TYPE::UT_LINUX_HTTP
#endif
    );
    uint16_t http_port = static_cast<uint16_t>(std::stoul(http_port_str));

    // Register login route
    http_server_.Post("/api/login", [](const gb::HttpRequest& req, gb::HttpResponse& res) {
        LOG_INFO("Login request: {}", req.body);
        // TODO: actual authentication logic
        // For now, echo back a success response
        res.SetJsonBody(R"({"code":0,"msg":"ok","data":{}})");
    });

    // Start HTTP server
    if (!http_server_.Start(http_ip, http_port, 2))
    {
        LOG_ERROR("LoginServer failed to start HTTP server on {}:{}", http_ip, http_port);
        return -1;
    }

    LOG_INFO("LoginServer HTTP listening on {}:{}", http_ip, http_port);
    return 0;
}

int LoginApp::OnCleanup()
{
    http_server_.Stop();
    return ServerApp::OnCleanup();
}
