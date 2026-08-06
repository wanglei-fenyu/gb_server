#pragma once
#include "app/server_app.h"
#include "network/http/server_http.hpp"
#include "network/io/io_service_pool.h"

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
    // HTTP 线程模型与 TCP 服务器统一（外部 IoServicePool 形式）：
    // acceptor 跑在专用维护线程，连接轮询分布到 io_context 池（每 io_context 一个线程）。
    // 成员声明顺序保证析构顺序：http_server_ 最先析构，池/维护线程比它活得久。
    gb::IoServicePoolPtr http_io_pool_;
    HttpServer           http_server_;
};
