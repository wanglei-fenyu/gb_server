#pragma once
#include "app/server_app.h"
#include "network/http/server_http.hpp"

class LoginApp : public ServerApp
{
public:
    using HttpServer = gb::http::Server<gb::http::HTTP>;

    LoginApp(int argc, char* argv[]);
    ~LoginApp() override = default;

protected:
    int OnServerInit() override;
    int OnCleanup() override;

private:
    HttpServer http_server_;
    std::thread http_server_thread_;
};
