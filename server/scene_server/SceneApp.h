#pragma once
#include "app/server_app.h"

class SceneApp : public ServerApp
{
public:
    SceneApp(int argc, char* argv[]);
    ~SceneApp() override = default;

protected:
    int OnServerInit() override;
};
