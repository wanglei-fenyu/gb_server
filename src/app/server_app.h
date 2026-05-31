#pragma once
#include "app/app.h"
#include "worker/worker_logic_interface.h"
#include "network/io/server.h"
#include "network/manager/network_manager.h"
#include <memory>

/// 默认工作线程逻辑——子类可以覆盖或替换
struct DefaultWorkerLogic : public gb::IWorkerLogic
{
    virtual int OnStartup() override;
    virtual int OnUpdate(float elapsed) override;
    virtual int OnTick() override;
    virtual int OnCleanup() override;
};

/// 所有游戏服务器进程（登录、网关、场景）的基类。
///
/// 封装了通用的启动流程：
///   - CLI 参数解析 (-t <APP_TYPE> -r <res_path>)
///   - Worker 创建 + Router 注册
///   - TCP 服务器启动（根据 APP_TYPE 从 server_config.xml 中查找 ip:port）
///   - NetworkManager 初始化
///   - 优雅生命周期钩子
class ServerApp : public App
{
public:
    /// @param argc,argv  来自 main()。期望 -t <type_value> -r <res_path>
    ServerApp(int argc, char* argv[]);
    virtual ~ServerApp() = default;

protected:
    // App lifecycle overrides
    virtual int OnInit() override;
    virtual int OnStartup() override;
    virtual int OnUpdate(float elapsed) override;
    virtual int OnTick() override;
    virtual int OnCleanup() override;
    virtual int OnUnInit() override;

    /// 子类可重写以注册自定义的 Listen/Register 处理器。
    /// 在 OnInit() 中，Worker 创建之后、Server::Start() 之前调用。
    virtual int OnServerInit() { return 0; }

protected:
    std::unique_ptr<gb::Server> server_;
};
