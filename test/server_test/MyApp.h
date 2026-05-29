#pragma once
#include "app/app.h"
#include "common/worker/worker.h"
#include "log/log_help.h"
#include "network/net/server.h"
#define  SERVER_APP true
struct NormalWorkerLogic : public gb::IWorkerLogic
{
	virtual int OnStartup();
	virtual int OnUpdate(float elapsed);
	virtual int OnTick();
	virtual int OnCleanup();

};
class MyApp :public App
{
public:
    MyApp(int argc, char* argv[]);
    ~MyApp(){};

protected:
	virtual int OnInit();
	virtual int OnStartup();
	virtual int OnUpdate(float);
	virtual int OnTick();
	virtual int OnCleanup();
    virtual int OnUnInit();

private:
    void init_http();

private:
    std::thread http_thread;
    //gb::http::HttpServerPtr http_server;

    std::unique_ptr<gb::Server> server_;

};

