#include "GatewayApp.h"

GatewayApp::GatewayApp(int argc, char* argv[])
    : ServerApp(argc, argv)
{
}

int GatewayApp::OnServerInit()
{
    LOG_INFO("GatewayServer::OnServerInit");
    // TODO: Register gateway RPC handlers / message listeners here
    // Gateway will forward messages to Scene Server via NATS (future)
    return 0;
}
