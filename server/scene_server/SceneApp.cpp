#include "SceneApp.h"

SceneApp::SceneApp(int argc, char* argv[])
    : ServerApp(argc, argv)
{
}

int SceneApp::OnServerInit()
{
    LOG_INFO("SceneServer::OnServerInit");
    // TODO: Register scene RPC handlers / message listeners here
    // e.g. gb::NetworkManager::Instance()->Listen(gb::MT_EnterScene, ...);
    return 0;
}
