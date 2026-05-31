#include "GatewayApp.h"

int main(int argc, char* argv[])
{
    GatewayApp app(argc, argv);
    app.Init();
    app.Run();
    return 0;
}
