#pragma once
#include "app/server_app.h"

class LoginApp : public ServerApp
{
public:
    LoginApp(int argc, char* argv[]);
    ~LoginApp() override = default;

protected:
    int OnServerInit() override;
    int OnCleanup() override;

private:
};
