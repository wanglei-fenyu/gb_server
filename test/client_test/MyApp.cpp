#include "MyApp.h"
#include "network/manager/network_manager.h"
#include "worker/worker_manager.h"
#include "test.h"
#include "base/res_path.h"
#include "cxxopts.hpp"
static bool is_net_init = false;


std::shared_ptr<gb::Client> g_client_;
async_simple::coro::Lazy<> test_coro_2(const gb::SessionPtr& session)
{
    try
    {
		gb::RpcCallPtr call = std::make_shared<gb::RpcCall>();
		call->SetSession(session);
		//co_await gb::CoRpc<>::execute(call, "test_rpc");

		auto r1 = co_await gb::CoRpc<int>::execute(call, "square", 1111, 10000);
		if (r1)
			LOG_INFO("CORO_TEST  {}", r1.value);
		else
			LOG_ERROR("CORO_TEST failed: {}", static_cast<int>(r1.error_code));

		auto r2 = co_await gb::CoRpc<int, std::string>::execute(call, "test_ret_args", 1111, 2, "world");
		if (r2)
		{
			auto [a, b] = r2.value;
			LOG_INFO("coro_test_2  {} {}", a, b);
		}
		else
			LOG_ERROR("coro_test_2 failed: {}", static_cast<int>(r2.error_code));

    }
    catch (...)
    {

		LOG_ERROR("unkonw");
    }
}


 MyApp::MyApp(int argc, char* argv[]) :App(argc,argv)
{
	cxxopts::Options options("client", "clien start");
    options.add_options()
        ("t,type", "process type",cxxopts::value<std::string>())
        ("r,res", "resource path", cxxopts::value<std::string>());

    auto result = options.parse(argc, argv);

    if (result.count("type"))
    {
        std::string typeStr   = result["type"].as<std::string>();
        int         typeValue = std::stoi(typeStr);
        appType_              = (APP_TYPE)typeValue;
    }

    if (result.count("res"))
    {
        std::string res_path = result["res"].as<std::string>();
        ResPath::Instance()->SetResRootPath(res_path);
    }
}

int MyApp::OnInit()
{
    //gb::WorkerManager* work_mng = gb::WorkerManager::Instance();
    gb::WorkerManager* work_mng = gb::WorkerManager::Instance();
    
    auto narmal_worker = work_mng->CreateWorker(std::make_shared<NormalWorkerLogic>(), gb::SWT_Normal);
    gb::NetworkManager::Instance()->GetRouter().RegisterWorker(gb::SWT_Normal, narmal_worker);

    gb::ClientOptions options;
    options.keep_alive_time = -1;
    client_.reset(new gb::Client(options));
    gb::NetworkManager::Instance()->Init(client_.get());
    
    Test_Register();
    
	client_->SetCloseCallBack([](const gb::SessionPtr session) {
        LOG_INFO("net close");
    });

	client_->SetConnnectCallBack([this](const gb::SessionPtr session) {
        LOG_INFO("net connect");
        is_net_init = true;
		g_client_ = client_;
        //session->StartHeartbeat(std::chrono::seconds(2));
        
        auto t1 = gb::WorkerManager::Instance()->GetWorker(0)->GetTimerManager()->RegisterTimer(
            6000, []() {
                LOG_ERROR("t1");
            },
            false);
        gb::WorkerManager::Instance()->GetWorker(0)->GetTimerManager()->UnRegisterTimer(t1);
        auto t2 = gb::WorkerManager::Instance()->GetWorker(0)->GetTimerManager()->RegisterTimer(
            2000, []() {
                LOG_ERROR("t2");
            },
            true);
        auto t3 = gb::WorkerManager::Instance()->GetWorker(0)->GetTimerManager()->RegisterTimer(
            10000, []() {
                LOG_ERROR("t3");
            },
           
            false);

        SendMsg1(client_);
        //gb::WorkerManager::Instance()->GetWorker(0)->Post([this]() {
        //   //async_simple::coro::syncAwait(test_coro_2(client_->GetSession(gb::CONNECT_TYPE::CT_GATEWAY)));
        //   // SendRpc(client_);
        //    test_coro_2(client_->GetSession(gb::CONNECT_TYPE::CT_GATEWAY)).start([](auto&&) {});
        //    });

        //http_test(client_);
    });

    

	auto [ip, port] = AppTypeMgr::Instance()->GetServerIpPort();
	std::string uir = ip + ":" + port;
    client_->Connect(gb::CONNECT_TYPE::CT_GATEWAY, uir);
    test_http();
    return 0;
}

int MyApp::OnStartup()
{
    gb::WorkerManager* work_mng = gb::WorkerManager::Instance();
    auto               workers  = work_mng->GetWorkers();
    for (auto worker : workers)
    {
        if (worker)
        {
			worker->Post([worker]() {worker->OnStartup();});
        }
    }
      
    return 0;
}

int MyApp::OnUpdate(float elapsed)
{
    return 0;
}

int MyApp::OnTick()
{
    return 0;

}

int MyApp::OnCleanup()
{
    gb::WorkerManager* work_mng = gb::WorkerManager::Instance();
    auto               workers  = work_mng->GetWorkers();
    for (auto worker : workers)
    {
        if (worker)
            worker->Post([worker]() { worker->OnCleanup(); });
    }
    return 0;
}

int MyApp::OnUnInit()
{
    client_->Shutdown();
    return 0;
}

void MyApp::test_http()
{

}


int NormalWorkerLogic::OnStartup()
{
    LOG_INFO(__FUNCTION__);
    gb::NetworkManager::Instance()->GetRouter().BindSingleEntity(1111, gb::WorkerManager::Instance()->GetCurWorker()->GetIndex());
    test_coro_2(g_client_->GetSession(gb::CONNECT_TYPE::CT_GATEWAY)).start([](auto&&) {});
    return 0;
}

int NormalWorkerLogic::OnUpdate(float elapsed)
{
    return 0;
}

int NormalWorkerLogic::OnTick()
{
    return 0;
}

int NormalWorkerLogic::OnCleanup()
{
    LOG_INFO(__FUNCTION__);
    return 0;
}
