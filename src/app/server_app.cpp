#include "server_app.h"
#include "worker/worker_manager.h"
#include "base/res_path.h"
#include "cxxopts.hpp"

// ---------------------------------------------------------------------------
// DefaultWorkerLogic — 默认工作线程逻辑
// ---------------------------------------------------------------------------
int DefaultWorkerLogic::OnStartup()
{
    LOG_INFO("{} (worker)", __FUNCTION__);
    return 0;
}
int DefaultWorkerLogic::OnUpdate(float /*elapsed*/) { return 0; }
int DefaultWorkerLogic::OnTick() { return 0; }
int DefaultWorkerLogic::OnCleanup()
{
    LOG_INFO("{} (worker)", __FUNCTION__);
    return 0;
}

// ---------------------------------------------------------------------------
// ServerApp — 服务器应用
// ---------------------------------------------------------------------------
ServerApp::ServerApp(int argc, char* argv[])
    : App(argc, argv)
{
    cxxopts::Options options("server", "game server");
    options.add_options()
        ("t,type", "process type", cxxopts::value<std::string>())
        ("r,res",  "resource path", cxxopts::value<std::string>());

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

int ServerApp::OnInit()
{
    // --- worker — 创建工作线程 ---
    auto* work_mng = gb::WorkerManager::Instance();
    auto  worker   = work_mng->CreateWorker(std::make_shared<DefaultWorkerLogic>(), gb::SWT_Normal);
    // 注册到 Router（Worker 自身已携带 service_type，此处保持显式注册便于后续扩展）
    gb::NetworkManager::Instance()->GetRouter().RegisterWorker(gb::SWT_Normal, worker);

    // --- network — 启动网络 ---
    auto [ip, port] = AppTypeMgr::Instance()->GetServerIpPort(appType_);
    std::string uri = ip + ":" + port;

    gb::ServerOptions opts;
    opts.keep_alive_time    = -1;
    opts.io_service_pool_size = 1;
    server_ = std::make_unique<gb::Server>(opts);

    server_->SetConnnectCallBack([](const gb::SessionPtr& session) {
        LOG_INFO("Accept:{}", session->socket().local_endpoint().address().to_string());
    });
    server_->SetCloseCallBack([](const gb::SessionPtr& session) {
        LOG_INFO("Close:{}", session->socket().local_endpoint().address().to_string());
    });

    gb::NetworkManager::Instance()->Init(server_.get());

    // 子类钩子
    if (int ret = OnServerInit(); ret != 0)
        return ret;

    server_->Start(uri);
    return 0;
}

int ServerApp::OnStartup()
{
    auto* work_mng = gb::WorkerManager::Instance();
    for (auto& w : work_mng->GetWorkers())
    {
        if (w) w->OnStartup();
    }
    return 0;
}

int ServerApp::OnUpdate(float /*elapsed*/) { return 0; }
int ServerApp::OnTick()                     { return 0; }

int ServerApp::OnCleanup()
{
    auto* work_mng = gb::WorkerManager::Instance();
    for (auto& w : work_mng->GetWorkers())
    {
        if (w) w->OnCleanup();
    }
    return 0;
}

int ServerApp::OnUnInit() { return 0; }
