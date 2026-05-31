#pragma once
#include "app/server_app.h"
#include "network/http/http_server.h"

class LoginApp : public ServerApp
{
public:
    LoginApp(int argc, char* argv[]);
    ~LoginApp() override = default;

protected:
    int OnServerInit() override;
    int OnCleanup() override;

private:
    gb::HttpServer http_server_;
};
