#pragma once
#include "app/server_app.h"

class GatewayApp : public ServerApp
{
public:
    GatewayApp(int argc, char* argv[]);
    ~GatewayApp() override = default;

protected:
    int OnServerInit() override;
};
