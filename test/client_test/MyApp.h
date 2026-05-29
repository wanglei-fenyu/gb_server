#pragma once
#include "app/app.h"
#include "network/net/client.h"
#include "network/net_manager/network_manager.h"
#include "common/worker/worker_logic_interface.h"
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
    void test_http();

private:
    //gb::http::HttpClientPtr     http_client;
    std::shared_ptr<gb::Client> client_;

};