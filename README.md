# gb_server
c++ 网络游戏服务器框架

# 编译
## windows
    1. 设置profile
    2. 执行 install_deps.bat
    3. conan install . -pr=profiles/msvc_debug_pr --build=missing
    4. cmake --preset conan-debug
    5. cmake --build --preset conan-debug --parallel 或者可以用vs打开cmake文件
    注意*: vs2026环境不会默认安装到系统环境，执行以上命令需要到  Command Prompt for VS下运行


## linux
    1. 设置profile,添加下面两行 conan有个bug Conan + Boost 1.90 的 recipe 在初始化阶段就不兼容 cobalt 模块, 取消boost charconv模块的float128支持
        [options]
        boost/*:without_cobalt=True
        [conf]
        tools.build:cxxflags+=["-DBOOST_CHARCONV_DISABLE_FLOAT128"]
    2. 执行 install_deps.sh
    3. conan install . -pr=profiles/clang_debug_pr --build=missing
    4. cmake --preset conan-debug
    5. cmake --build --preset conan-debug --parallel

---

# 总览

## 可执行文件

| 目标 | 入口 | 基类 | 用途 |
|---|---|---|---|---|
| `server_test` | `test/server_test/main.cpp` | `App` | 开发测试服务端 |
| `client_test` | `test/client_test/main.cpp` | `App` | 开发测试客户端 |
| `db_test` | `test/db_test/main.cpp` | 无（独立控制台） | 数据库/Redis 集成测试 |
| `login_server` | `server/login_server/main.cpp` | `ServerApp` | 登录服务器 |
| `gateway_server` | `server/gateway_server/main.cpp` | `ServerApp` | 网关服务器 |
| `scene_server` | `server/scene_server/main.cpp` | `ServerApp` | 场景服务器 |

所有二进制都通过 glob 包含 `src/` 下**所有源文件**，不存在按需选择。

## App 类型体系

```
App (src/app/app.h)                     — 帧循环、生命周期、优雅关闭
 └─ ServerApp (src/app/server_app.h)    — CLI 解析、Worker/Network 自动初始化
      ├─ LoginApp                       — 登录服务器（带 HttpServer）
      ├─ GatewayApp                     — 网关服务器
      └─ SceneApp                       — 场景服务器
```

---

# 模块一：生命周期（App）

## 启动流程

```
main(argc, argv)
  └─ MyApp app(argc, argv)
       └─ app.Init()
            ├─ log.Init()                           — spdlog 初始化
            ├─ WorkerManager::InitMainWorker()       — 创建主 Worker
            ├─ ShutdownManager::Initialize()         — 注册 4 阶段关闭回调
            ├─ SignalHandler::Initialize()           — Ctrl+C / SIGTERM
            ├─ OnInit()                              — ★ 虚函数，应用层初始化
            │    ├─ [ServerApp] 解析 -t -r CLI 参数
            │    ├─ [ServerApp] 创建 NormalWorker + 注册 Router
            │    ├─ [ServerApp] 创建 Server、启动 TCP
            │    └─ OnServerInit()                   — ★ 子类钩子
            └─ runding_ = true

app.Run()
  ├─ OnStartup()                                     — ★ 所有 Worker 启动
  │    └─ ServerApp::OnStartup()
  │         └─ worker->OnStartup()
  │              └─ Worker::OnStart() → InitLua()
  │                   ├─ open_libraries()
  │                   ├─ _lua_(scriptPtr)            — 注册 C++ 绑定
  │                   ├─ require("socket.core")
  │                   ├─ start_debug.lua             — LuaPanda 调试器
  │                   └─ require("main.lua")         — ★ 用户 Lua 脚本
  ├─ NetworkManager::Freeze()                        — Lock 处理器为只读原子快照
  └─ 帧循环（见下方）
       ├─ OnUpdate(elapsed)
       ├─ main_worker->ProcessFrame(elapsed)
       └─ OnTick()
```

## 帧循环

```cpp
// App::Run()
while (runding_) {
    if (SignalHandler::IsSignalReceived()) break;

    current_time = steady_clock::now();
    elapsed = current_time - last_time;
    last_time = current_time;

    // 1. 应用层更新
    OnUpdate(elapsed);

    // 2. 主 Worker 帧处理
    main_worker->ProcessFrame(elapsed);

    // 3. 应用层 Tick
    OnTick();

    // 4. 帧率控制（默认 60fps ≈ 16ms）
    frame_time = now - current_time;
    if (frame_time < frame_duration_)
        sleep_for(frame_duration_ - frame_time);
}
```

```cpp
// Worker::ProcessFrame(float elapsed)
void Worker::ProcessFrame(float elapsed) {
    OnUpdate(elapsed);    // 定时器派发 + 队列执行 + WorkerLogic
    OnTick();              // WorkerLogic::OnTick
}

// Worker::OnUpdate(float elapsed)
void Worker::OnUpdate(float elapsed) {
    // 1. 派发到期的定时器
    timer_manager_->Update();

    // 2. 执行队列中所有待处理任务
    while (events_.try_dequeue(func)) {
        func();
    }

    // 3. 调用用户逻辑
    if (worker_logic_)
        worker_logic_->OnUpdate(elapsed);
}
```

## 优雅关闭（4 阶段）

```
触发：Ctrl+C / SIGINT / SIGTERM
  │
  └─ App::Stop() → runding_=false → 主循环退出
       └─ ShutdownManager::Shutdown()
            │
            ├─ Phase 1: StoppingIO
            │    └─ IoServicePool::GracefulStop()
            │        → 所有 IoWorker 停止 accept/read/write
            │
            ├─ Phase 2: CompletingTimers
            │    ├─ 所有 Worker::TimerManager::EnterShutdownMode()
            │    └─ 主 Worker 执行最后一次 ProcessFrame()
            │
            ├─ Phase 3: ProcessingTasks
            │    ├─ 所有 Worker::EnterShutdownMode()
            │    │   → shutting_down_=true, 拒绝新任务入队
            │    └─ 最多等待 5s，直到队列为空
            │
            └─ Phase 4: Cleaning
                 ├─ App::OnCleanup()
                 ├─ 每个 Normal Worker 执行 CleanupInWorkerThread(5s)
                 │   超时则直接 Stop()
                 ├─ join 所有 worker 线程
                 ├─ 主 Worker::OnCleanup()
                 ├─ App::OnUnInit()
                 └─ SignalHandler::Cleanup()
```

---

# 模块二：线程模型 & Worker 系统

## 线程模型

```
┌─────────────────────────────────────────────────────────────────┐
│  Main Thread (App::Run 帧循环)                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  Main Worker — ProcessFrame(elapsed)                     │   │
│  │  - TimerManager::Update()（稳态 + 系统定时器）             │   │
│  │  - 执行队列任务 (ConcurrentQueue)                          │   │
│  │  - WorkerLogic::OnUpdate / OnTick                         │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                 │
│  Normal Worker(s) — 独立线程，内部帧循环                          │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  while (runing_):                                        │   │
│  │    wait_for(50ms) 或新任务到来                            │   │
│  │    ProcessFrame(elapsed)                                  │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                 │
│  IO Thread Pool (IoServicePool)                                 │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  IoWorker × N                                             │   │
│  │  每个 IoWorker 运行自己的 io_context::run()                │   │
│  │  处理：accept、read、write、SSL handshake、心跳            │   │
│  │  收到完整消息 → OnReceiveCall → Dispatch → Router          │   │
│  │  → 投递到目标 Worker                                      │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

## Worker 帧处理

| Worker 类型 | 所在线程 | 帧驱动方式 | 用途 |
|---|---|---|---|
| **Main Worker** | Main 线程 | App::Run 帧循环中直接调用 `ProcessFrame()` | 定时器、队列任务、业务逻辑 |
| **Normal Worker** | 独立线程 | 内部 `while(runing_)` 循环，50ms 等待或新任务唤醒 | 按路由分配的异步消息处理 |
| **IoWorker** | 独立线程 | `io_context::run()` 事件驱动 | 网络 IO（accept/read/write） |

```cpp
// Normal Worker 线程循环
void Worker::Run() {
    while (true) {
        if (!runing_) {
            if (!events_.try_dequeue(func)) break;
            func();  // 关闭时继续处理剩余任务
            continue;
        }
        event_cv_.wait_for(lock, 50ms,
            [this]() { return !runing_ || events_.size_approx() > 0; });
        ProcessFrame(elapsed);
    }
}
```

## 任务投递

```cpp
// 任何线程向指定 Worker 投递任务
worker->Post([this]() {
    // 在 Worker 线程上下文中执行
});

// 或通过 WorkerExecutor（由 Router 获取）
auto executor = router.GetServiceExecutor(msg_type, route_id);
executor.Dispatch([]() {
    // 自动路由到目标 Worker
});
```

## Worker 关键 API

```cpp
class Worker {
    void Post(const std::function<void(void)>& handler);   // 投递任务
    void ProcessFrame(float elapsed);                      // 帧处理
    ScriptPtr GetScript();                                 // 获取 Lua 状态
    std::unique_ptr<TimerManager>& GetTimerManager();      // 定时器管理器
    uint32_t AllocRpcSeq();                                // 分配 RPC 序列号
    void StorePendingRpc(uint32_t local_seq, RpcCallPtr);  // 存储待处理 RPC
    static RpcCallPtr TakePendingRpc(uint32_t local_seq);  // 取出待处理 RPC
};
```

---

# 模块三：消息系统

## Meta 消息头

每个网络消息前固定一个二进制 `Meta` 结构体：

```cpp
struct Meta {
    MsgMode      mode{Msg};         // Msg / Request / Response
    uint64_t     id{0};             // 用户标识（路由 key）
    uint32_t     type{0};           // 消息类型（Listen 分发用）
    CompressType compress_type{CompressTypeNone};
    uint64_t     method{0};         // RPC 方法名 MD5 哈希
    uint64_t     sequence{0};       // RPC 序列号（编码 worker_index+local_seq）
};
```

Meta 后紧跟 protobuf 序列化的消息体（普通消息）或 msgpack 序列化的参数（RPC 消息）。

## 发送消息

```cpp
// C++ 发送 protobuf 消息
NetworkManager::Instance()->Send(session, type, id, protoMsg);

// Lua 发送 protobuf 消息
net.Send(session, type, id, "ProtoName", lua_msg)
```

## 注册与接收消息（Listen）

```cpp
// C++ 注册消息处理器
NetworkManager::Instance()->Listen(type, [](const SessionPtr& session,
    const ReadBufferPtr& buffer, Meta& meta, int meta_size, int64_t data_size) {
    // 解析 protobuf 消息...
});

// Lua 注册消息处理器
net.Listen(type, function(session, message)
    -- message 是 protobuf 对象
end, "ProtoName")
```

## 服务器端

```cpp
// ServerApp 在 OnInit() 中自动完成
gb::ServerOptions opts;
opts.io_service_pool_size = 1;
server_ = std::make_unique<gb::Server>(opts);

server_->SetConnnectCallBack([](const SessionPtr& session) { ... });
server_->SetCloseCallBack([](const SessionPtr& session) { ... });

gb::NetworkManager::Instance()->Init(server_.get());
server_->Start("ip:port");
```

## 消息处理完整流程

```
Client → TCP → Server::Impl → Listener → Session
  │
  └─ on_received(buffer, meta_size, data_size)
       └─ Session::_received_callback
            └─ NetworkManager::OnReceiveCall
                 └─ NetworkManager::Dispatch
                      ├─ MsgMode::Msg      → FindListenFunction(type)
                      │                       → CreateExecutorForRoute(type, id)
                      │                       → WorkerExecutor::Dispatch → Worker::Post
                      │
                      ├─ MsgMode::Request   → FindRpcFunction(method)
                      │                        → CreateExecutorForRoute(type, id)
                      │                        → WorkerExecutor::Dispatch → Worker::Post
                      │
                      └─ MsgMode::Response  → 从 sequence 解码 WorkerIndex + LocalSeq
                                              → Worker::Post → TakePendingRpc(local_seq)
                                              → RpcCall::Done → 触发回调
```

---

# 模块四：路由机制

## 路由总览

gb_server 有三种完全不同的路由机制：

| 消息类型 | 路由依据 | 路由目标 | 调度方式 |
|---|---|---|---|
| **普通消息 (Msg)** | `meta.type` (uint32) | `ServiceWorkerType` → Worker | `Router::GetServiceExecutor` |
| **RPC 请求 (Request)** | `meta.method` (MD5) | `ServiceWorkerType` → Worker | `Router::GetServiceExecutor` |
| **RPC 响应 (Response)** | `meta.sequence` (WorkerIndex) | 直接投递指定 Worker | 从 sequence 解码，不经过 Router |
| **HTTP 请求** | URL path + HTTP method | 匹配 RouteEntry handler | 线性遍历 routes vector |

## 普通消息 & RPC 请求路由

```
  meta.type → ResolveServiceWorkerType(MessageType)
       │
       │  MessageType % 10000:
       │     0       → Normal
       │     10001   → AI
       │     20001   → Navigation
       ▼
  ServiceWorkerType → GetWorker(type) → Worker vector
       │
       ▼
  PickWorker(workers, route_id)
       │  默认: route_id % workers.size()
       │  可自定义: SetWorkerIndexSelector(...)
       ▼
  WorkerExecutor::Dispatch(func) → Worker::Post → Worker::ProcessFrame
```

**MessageType 定义：**

```cpp
enum MessageType : uint32_t {
    MT_Login          = 0,       // 10000→ Normal
    MT_EnterScene     = 1,       // → Normal
    MT_AI_Run         = 10000,   // 10000%10000=0 → Normal
    MT_AI_Skill       = 10001,   // 10001%10000=1 → AI
    MT_AStar          = 20000,   // 20000%10000=0 → Normal
    MT_LineFindPath   = 20001,   // 20001%10000=2 → Navigation
};
```

**ServiceWorkerType：**

| 枚举值 | 名称 | 消息区间 |
|---|---|---|
| `SWT_Normal = 0` | 普通业务 | `% 10000 == 0` |
| `SWT_AI = 1` | AI 业务 | `% 10000 == 1` |
| `SWT_Navigation = 2` | 寻路业务 | `% 10000 == 2` |

**自定义路由策略：**

```cpp
// 自定义消息类型 → ServiceWorkerType 映射
router.SetServiceTypeResolver([](MessageType type) -> ServiceWorkerType {
    if (type >= 10000 && type < 20000) return SWT_AI;
    return SWT_Normal;
});

// 自定义 Worker 选择（默认 route_id % workers.size()）
router.SetWorkerIndexSelector([](auto& workers, auto type, auto route_id) {
    return route_id % workers.size();
});
```

## RPC 响应路由（零查找直投）

RPC 响应路由是**零查找、零锁**的直投机制——直接从 `meta.sequence` 中解码目标 Worker：

```
  meta.mode == Response
     │
     ▼
  SequenceId sid;
  sid.value = meta.sequence;
  worker_index = sid.index;  // 高 32 位
  local_seq    = sid.seq;    // 低 32 位
     │
     ▼
  WorkerManager::GetWorker(worker_index)
     │
     ▼
  worker->Post([local_seq]() {
      auto call = Worker::TakePendingRpc(local_seq);
      call->Done(session, buffer, meta, ...);
  });
```

**Sequence 编码：**

```cpp
union SequenceId {
    struct { uint64_t index : 32; uint64_t seq : 32; };
    uint64_t value;
};

// 请求时编码：meta.sequence = (worker_index << 32) | local_seq
// 响应时解码：worker_index = sequence >> 32, local_seq = sequence & 0xFFFFFFFF
```

这个过程完全在 IO 线程完成，不访问任何共享映射表。

## HTTP 路由

HTTP 路由**完全独立**，基于 Boost.Beast，不与 `Router`/`NetworkManager` 共享逻辑。

```
  TCP 连接 → Listener → HttpSession / HttpsSession → SSL Handshake
     │
     ▼
  beast_http::async_read() 解析 HTTP 请求
     │
     ▼
  HttpServer::Impl::Dispatch(path, method, req, res)
     ├─ 快照 routes_ vector
     ├─ 遍历精确匹配 route.path == path && route.method == method
     │    └─ 执行 handler(req, res)
     └─ 未匹配 → 404
     │
     ▼
  beast_http::async_write() 发送响应
```

```cpp
// 精确字符串匹配，/api/user 与 /api/user/ 不同
srv.Get("/api/user", handler);
srv.Post("/api/login", handler);
srv.AddRoute("/data", boost::beast::http::verb::put, handler);
```

**三种路由对比：**

| 特性 | 普通消息 | RPC | HTTP |
|---|---|---|---|
| 路由依据 | `meta.type` (uint32) | `meta.method` (MD5) / `meta.sequence` | URL path + method |
| 路由表 | `ListenMap` (type→func) | `RpcInterfaceMap` (hash→func) | `vector<RouteEntry>` |
| Worker 调度 | Router 分配 | Request: Router / Response: 直投 | 无（IO 线程直接执行） |
| 处理器查找 | `Freeze()` 后无锁原子指针 | `Freeze()` 后无锁原子指针 | 遍历 vector（mutex+快照） |
| 序列化 | protobuf | msgpack 或 protobuf | JSON/文本/任意 Body |
| 线程模型 | IO → Worker 队列 | IO → Worker 队列 | IO 线程直接执行 |

---

# 模块五：RPC 系统

## 概览

RPC 使用 **msgpack** 序列化参数（或 protobuf 单消息参数），**MD5 哈希**（`MD5::MD5Hash64`）作为方法标识。支持超时、取消、回调、协程（`CoRpc`）。

## 服务端注册 RPC

```cpp
// 基本形式
NetworkManager::Instance()->Register("method_name",
    [](RpcReply reply, int arg1, std::string arg2) {
        reply.Invoke(result);
    });

// 无参数、无返回值
NetworkManager::Instance()->Register("method_name", []() {
    // 处理请求
});

// protobuf 参数（自动从流中反序列化）
NetworkManager::Instance()->Register("method_name",
    [](RpcReply reply, MyProtoMsg msg) {
        // 处理
    });
```

## 客户端调用 RPC

```cpp
// ─── 回调风格 ───
auto call = std::make_shared<RpcCall>();
call->SetCallBack([](int result) {
    // 处理响应
});
call->SetTimeout([]() {
    // 超时处理
}, 5000);
call->SetSession(session);                  // 绑定会话（可选）
NetworkManager::Instance()->Call(call, "method_name", arg1, arg2);

// ─── 协程风格（CoRpc）───
// CoRpc<T>::execute(call, method, args...) → async_simple::Lazy<T>
auto result = co_await CoRpc<int>::execute(
    std::make_shared<RpcCall>(), "method_name", arg1, arg2);

// 多返回值协程
auto [a, b, c] = co_await CoRpc<int, std::string, float>::execute(
    std::make_shared<RpcCall>(), "multi_return", arg1);

// 无返回值协程
co_await CoRpc<void>::execute(
    std::make_shared<RpcCall>(), "void_method");
```

## 完整 RPC 流程

```
┌─ 调用端 (Worker 线程) ─────────────────────────────────┐
│                                                         │
│  NetworkManager::Call(call, "method", args...)            │
│    1. MD5::MD5Hash64("method") → method_key              │
│    2. msgpack::pack(args...) → buffer                    │
│    3. local_seq = worker->AllocRpcSeq()                  │
│    4. meta.sequence = encode(worker_index, local_seq)    │
│    5. worker->StorePendingRpc(local_seq, call)           │
│       └─ thread_local unordered_map（无锁）              │
│    6. call->Call(meta, buffer) → 发送                    │
└─────────────────────────────────────────────────────────┘
                          │ TCP
                          ▼
┌─ 对端 (IO 线程 → Worker) ───────────────────────────────┐
│                                                         │
│  NetworkManager::Dispatch()                              │
│    meta.mode == Request                                  │
│    1. FindRpcFunction(meta.method) 查处理器              │
│    2. CreateExecutorForRoute(meta.type, meta.id)         │
│    3. WorkerExecutor::Dispatch → Worker::Post            │
│    4. Worker 执行处理器, reply.Invoke(data)              │
│       └─ 发送 Response 回调用端                          │
└─────────────────────────────────────────────────────────┘
                          │ TCP
                          ▼
┌─ 响应 (IO 线程 → Worker) ───────────────────────────────┐
│                                                         │
│  NetworkManager::Dispatch()                              │
│    meta.mode == Response                                 │
│    1. sequence 解码 → worker_index, local_seq            │
│    2. WorkerManager::GetWorker(worker_index)              │
│    3. worker->Post([local_seq] {                         │
│         auto call = Worker::TakePendingRpc(local_seq);    │
│         call->Done(...) → 触发回调/恢复协程              │
│       })                                                 │
└─────────────────────────────────────────────────────────┘
```

## 关键类

| 类 | 作用 |
|---|---|
| `RpcCall` | RPC 请求对象，管理超时/取消/回调/状态 |
| `RpcReply` | RPC 响应对象，服务端通过它 `Invoke()` 返回数据 |
| `CoRpc<T>` | 协程包装器，将 RPC 调用包装为 `async_simple::Lazy<T>` |
| `RpcCallAwaiter<T>` | `co_await` 可等待对象，内部管理 RpcAwaitState |
| `SequenceId` | union 编码 worker_index(32) + local_seq(32) |
| `ThreadLocalRpcContext` | 每个线程独立的 RPC 待处理映射（无锁） |

---

# 模块六：Lua 脚本系统

## 架构

每个 Worker 拥有**独立的 Lua 状态**（sol::state），`main.lua` 在每个 Worker 线程上各加载一次。

```
Worker::OnStart()
  └─ InitLua()
       ├─ sol::state::open_libraries()     — 打开所有 Lua 标准库
       ├─ _lua_(scriptPtr)                 — 注册 C++ 绑定到 Lua
       │    ├─ register_log()              — log.Info/Error/Warning
       │    ├─ register_msgpack()          — msgpack.pack/unpack
       │    ├─ register_proto_msg()        — protobuf 消息类型注册
       │    └─ register_net()              — net.Listen/Register/Send/Call
       ├─ require("socket.core")           — LuaSocket
       ├─ start_debug.lua                  — LuaPanda 调试器（可选）
       └─ require("main.lua")              — ★ 用户 Lua 脚本
            ├─ net.Listen(type, func, "ProtoName")
            ├─ net.Register("method", func)
            └─ (业务逻辑)
```

## Freeze 机制

所有 Worker 的 `InitLua()` 完成后，`App::Run()` 调用：

```cpp
NetworkManager::Instance()->Freeze();
```

Freeze 将 `ListenMap` 和 `RpcInterfaceMap` 快照为**只读原子指针**：

```
Freeze() 前：                  Freeze() 后：
┌──────────────────┐          ┌──────────────────┐
│ ListenMap        │          │ frozen_listen_map_│ ← atomic<const ListenMap*>
│ RpcInterfaceMap  │          │ frozen_rpc_       │
│ (可变，mutex 保护)│   →     │ interface_map_    │ ← atomic<const RpcInterfaceMap*>
└──────────────────┘          └──────────────────┘
                                   只读，无锁访问
```

- `Listen` 和 `Register` 必须在 `Freeze()` **之前**完成
- `Freeze()` 后 `FindListenFunction` / `FindRpcFunction` 变为**无锁访问**
- 每个 Worker 的 `thread_local` RPC 待处理映射处理所有响应路由

## 消息处理器注册（Lua）

```lua
-- 网络消息处理器
net.Listen(type, function(session, message)
    -- message 是 protobuf 对象，由第三个参数指定类型
    log.Info("received message type:" .. type)
end, "ProtoName")
```

## RPC 注册与调用（Lua）

```lua
-- ─── 服务端注册 RPC ───
-- 无参数
function lua_rpc_test(reply)
    log.Warning("lua_rpc_test called")
end
net.Register("lua_rpc_test", lua_rpc_test)

-- 带参数，通过 reply:Invoke 返回
function lua_rpc_test_args(reply, a)
    log.Warning("lua_rpc_test_args:" .. a)
    reply:Invoke(a .. a)  -- a="123" → 返回 "123123"
end
net.Register("lua_rpc_test_args", lua_rpc_test_args)

-- 多参数
function rpc_multi_args(reply, id, name, score)
    log.Info(string.format("id=%d, name=%s, score=%.1f", id, name, score))
    reply:Invoke("ok")
end
net.Register("rpc_multi_args", rpc_multi_args)

-- ─── 客户端发起 RPC 调用 ───
local call = RpcCall.new()
call:SetSession(session)
call:SetCallBack(function(reply, result)
    log.Info("RPC response: " .. result)
end)
net.Call(call, "lua_rpc_test_args", "hello")
```

## RPC 参数类型映射

RPC 使用 **msgpack** 序列化参数，Lua 与 C++ 的类型对应：

| Lua 类型 | msgpack 类型 | C++ 对应类型 |
|---|---|---|
| `number` | integer/float | `int`, `int64_t`, `float`, `double` |
| `string` | string | `std::string` |
| `boolean` | bool | `bool` |
| `table`（数组部分） | array | `std::vector<T>` |
| `table`（字典部分） | map | `std::map<K,V>` |

## Lua API 参考

| API | 说明 |
|---|---|
| `net.Listen(type, func, "ProtoName")` | 注册消息处理器，type 匹配 MessageType，第三个参数指定 protobuf 类型名 |
| `net.Register("method", func)` | 注册 RPC 方法 |
| `net.Send(session, type, id, "ProtoName", msg)` | 发送 protobuf 消息 |
| `net.Call(call, "method", ...)` | 发起 RPC 调用 |
| `log.Info(str)` / `log.Error(str)` / `log.Warning(str)` | 日志（带源码位置追踪） |
| `msgpack.pack(...)` | msgpack 序列化 |
| `msgpack.unpack(data)` | msgpack 反序列化 |
| `create_msg("ProtoName")` | 创建 protobuf 消息对象 |
| `RpcCall.new()` | 创建 RPC 调用对象 |
| `RpcCall:SetSession(session)` | 绑定会话 |
| `RpcCall:SetCallBack(func)` | 设置回调函数，`func(返回值...)` |
| `RpcReply:Invoke(...)` | 在 RPC 处理器中返回响应 |

## Lua 注意事项

1. **每个 Worker 独立 Lua 状态**：`net.Listen` 和 `net.Register` 在每个 Worker 线程各调用一次
2. **注册必须在 Freeze 之前**：所有 Lua 初始化完成后才会调用 `Freeze()`，在此之前必须完成所有注册
3. **Lua 的 protobuf 桥接**：通过 `lua_pb_parse.h` 实现 protobuf ↔ Lua table 的双向转换
4. **调试支持**：`script/start_debug.lua` 启用 LuaPanda 调试器（`127.0.0.1:8828`）

---

# 模块七：HTTP 模块

基于 **Boost.Beast**，支持 HTTP/1.1 和 HTTPS。

## HttpServer

```cpp
#include "network/http/http_server.h"

gb::HttpServer srv;

// 注册路由
srv.Get("/api/hello", [](const gb::HttpRequest& req, gb::HttpResponse& res) {
    res.SetJsonBody(R"({"code":0,"msg":"hello"})");
});

srv.Post("/api/login", [](const gb::HttpRequest& req, gb::HttpResponse& res) {
    std::string body = req.body;
    res.SetJsonBody(R"({"code":0})");
});

// 启动
srv.Start("0.0.0.0", 8080, 2);  // IP, Port, 线程数

// 或 HTTPS
srv.StartSSL("0.0.0.0", 443, "server.crt", "server.key", 2);

// 停止
srv.Stop();
```

**路由规则：** 精确字符串匹配，`/api/user` 和 `/api/user/` 视为不同路径。

**Query 参数自动解析：**
```cpp
srv.Get("/api/user", [](const gb::HttpRequest& req, gb::HttpResponse& res) {
    std::string id = req.GetParam("id");  // URL: /api/user?id=123
});
```

## HttpRequest / HttpResponse

```cpp
struct HttpRequest {
    boost::beast::http::verb method;
    std::string              target;        // URL（含Query）
    std::string              body;          // 请求体
    std::string              content_type;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> params;  // Query 参数

    std::string GetHeader(const std::string& key) const;
    std::string GetParam(const std::string& key) const;
};

struct HttpResponse {
    int    status = 200;
    std::string body;
    std::string content_type = "application/json";

    void SetHeader(const std::string& key, const std::string& value);
    void SetJsonBody(const std::string& json);
    void SetTextBody(const std::string& text, const std::string& type = "text/plain");
};
```

## HttpClient

支持**协程**（`asio::awaitable`）和**回调**两种 API。

```cpp
#include "network/http/http_client.h"

boost::asio::io_context ioc;

// ─── 协程 API ───
boost::asio::awaitable<void> DoHttp() {
    gb::HttpClient client(ioc);
    client.SetTimeouts({
        .connect_timeout_seconds = 5,
        .read_timeout_seconds    = 10,
        .write_timeout_seconds   = 10
    });

    auto res = co_await client.Get("https://api.example.com/users");
    LOG_INFO("status={} body={}", res.status, res.body);

    res = co_await client.Post("https://api.example.com/login",
        R"({"user":"alice","pass":"123"})", "application/json");
}

boost::asio::co_spawn(ioc, DoHttp, boost::asio::detached);
ioc.run();

// ─── 回调 API ───
gb::HttpClient client(ioc);
client.Get("http://example.com/api", [](gb::HttpResponse res) {
    if (res.status == 200) {
        LOG_INFO("Success: {}", res.body);
    }
});
```

URL 协议头自动识别：`http://` → 普通 HTTP，`https://` → SSL/TLS。

---

# 模块八：定时器系统

每个 Worker 拥有独立的 `TimerManager`，使用**双优先级队列**（最小堆）：

- **SteadyTimer** — 基于 `steady_clock`，适合游戏逻辑定时（不受系统时间影响）
- **SystemTimer** — 基于 `system_clock`，适合需要对齐现实时间的场景

```cpp
// 注册一次性定时器（500ms 后执行）
timer_manager_->RegisterTimer(500, []() {
    LOG_INFO("Timer fired!");
});

// 注册循环定时器（每 1000ms 执行一次）
timer_manager_->RegisterTimer(1000, []() {
    LOG_INFO("Loop timer fired!");
}, true);

// 取消定时器
timer_manager_->UnRegisterTimer(timerId);
```

定时器在 `Worker::OnUpdate()` 中的 `TimerManager::Update()` 中每帧派发一次到期的定时器。

---

# 模块九：错误处理与日志

## 日志系统

基于 **spdlog**，支持异步模式、控制台+文件输出、日志轮转：

```cpp
// 初始化（App::Init 中自动完成）
log.Init("log4/test.log", 1024*1024*1000, 10,
         GbLog::ASYNC, GbLog::CONSOLE_AND_FILE, GbLog::LEVEL_INFO);

// 日志宏（可变参数 fmt 风格）
LOG_INFO("Player {} logged in from {}", player_id, ip);
LOG_ERROR("Failed to connect: {}", err_msg);
LOG_WARN("Timeout on session {}", session_id);
LOG_DEBUG("Tick count: {}", count);
LOG_CRITI("Fatal error: {}", msg);
```

## 错误处理惯例

- 返回 `0` = 成功，`-1` = 失败
- `CHECK(expr)` / `CHECK_EQ(a,b)` — 失败时 LOG_ERROR，不 abort
- 所有虚函数生命周期钩子：`OnXxx()` 返回 `int`，非零触发提前退出

---

# 模块十：Redis Lua 接口

基于 **Boost.Redis**（连接池 + RESP3 协议），支持异步回调与 Lua 协程两种调用风格。通过 `redis.Connect()` 初始化连接池。

## 连接管理

```lua
-- 初始化 Redis 连接池（在 main.lua 中调用一次）
redis.Connect({
    host      = "127.0.0.1",
    port      = 6379,
    password  = "",
    db        = 0,
    pool_size = 4,
    timeout   = 5000,       -- 毫秒
})

-- 检查连接池健康
local ok = redis.IsHealthy()   -- → boolean
```

## 异步回调 API

所有异步方法通过 `callback(err, value)` 返回值。`err` 为空字符串表示成功，非空为错误描述。

### String 操作

```lua
redis.AsyncSet("key", "value", function(err) end)
redis.AsyncSetEx("key", "value", 60, function(err) end)     -- 60s 过期
redis.AsyncGet("key", function(err, val) end)               -- val=nil 表示 key 不存在
redis.AsyncDel("key", function(err, n) end)                 -- n=删除数量
redis.AsyncExists("key", function(err, n) end)
redis.AsyncIncr("key", function(err, n) end)                -- 自增 1
redis.AsyncIncrBy("key", 10, function(err, n) end)          -- 自增 10
redis.AsyncExpire("key", 60, function(err) end)
redis.AsyncTTL("key", function(err, ttl) end)
```

### Hash 操作

```lua
redis.AsyncHSet("hash", "field", "value", function(err) end)
redis.AsyncHGet("hash", "field", function(err, val) end)
redis.AsyncHDel("hash", "field", function(err) end)
redis.AsyncHKeys("hash", function(err, keys) end)           -- keys: table array
redis.AsyncHVals("hash", function(err, vals) end)           -- vals: table array
redis.AsyncHLen("hash", function(err, n) end)
```

### List 操作

```lua
redis.AsyncLPush("list", "value", function(err, n) end)
redis.AsyncRPush("list", "value", function(err, n) end)
redis.AsyncLPop("list", function(err, val) end)
redis.AsyncRPop("list", function(err, val) end)
redis.AsyncLLen("list", function(err, n) end)
```

### Sorted Set 操作

```lua
redis.AsyncZAdd("zset", 1.0, "member", function(err) end)
redis.AsyncZRange("zset", 0, -1, function(err, members) end)
redis.AsyncZRevRange("zset", 0, -1, function(err, members) end)
redis.AsyncZCard("zset", function(err, n) end)
redis.AsyncZRem("zset", "member", function(err) end)
redis.AsyncZScore("zset", "member", function(err, score) end)
redis.AsyncZRank("zset", "member", function(err, rank) end)
```

### 通用命令

```lua
-- 发送任意 Redis 命令（RESP3 自动解析返回值）
redis.AsyncCall("CLUSTER", "INFO", function(err, val) end)

-- 执行 Lua 脚本
redis.AsyncEval("return redis.call('GET', KEYS[1])", 1, "mykey",
    function(err, val) end)

-- Ping
redis.AsyncPing(function(err) end)
```

## 协程桥接 — redis.Await

允许在 Lua 协程中以**同步风格**写异步代码：

```lua
local co = coroutine.create(function()
    local err, val = redis.Await("Get", "mykey")
    if err ~= "" then
        log.Error("redis error: " .. err)
        return
    end
    log.Info("got: " .. tostring(val))
end)
coroutine.resume(co)
```

`redis.Await(method, ...)` 调用对应的 `redis.Async<method>`，自动处理回调与协程的 yield/resume 桥接。

**原理：** `sol::function` 在协程上下文创建时引用协程的 `lua_State`。若直接在协程栈上执行回调，会与 `coroutine.yield/resume` 产生冲突。Lua 回调桥接通过 `lua_xmove` 将函数引用迁移到主线程的 `lua_State` 上执行（详见 `register_redis.cpp` 的 `LuaCbBridge::Create`）。

---

# 模块十一：PostgreSQL Lua 接口

基于 **libpq** 异步接口 + Boost.Asio reactor 模式。通过专用 PG IO 线程处理所有异步操作，回调通过 `Worker::Post` 回到发起调用的 Worker 线程。

## 架构

```
Worker Thread                    PG IO Thread             PostgreSQL
     │                               │                       │
     ├─ pg.AsyncQuery(sql, cb) ──────┤                       │
     │                               ├─ PQsendQuery() ───────┤
     │                               │       ··· async ···   │
     │                               ├─ PQgetResult() ←──────┤
     │  w->Post([cb] { cb(res) }) ←──┤                       │
     ▼                               ▼                       ▼
  Worker 帧循环                   io_context::run()        libpq
```

- 全局唯一 PG IO 线程（`g_pg_io_ctx.run()`）
- 全局连接通过 `pg.AsyncConnect` 建立，后续所有调用共享该连接
- 回调始终回到**发起调用的 Worker 线程**（通过 `Worker::Post`）

## 连接管理

```lua
-- 异步连接 PostgreSQL
pg.AsyncConnect({
    host            = "127.0.0.1",
    port            = 5432,
    database        = "test",
    user            = "postgres",
    password        = "",
    use_ssl         = false,
    connect_timeout = 10,         -- 秒
}, function(err, ok)
    if ok then
        log.Info("PG connected!")
    end
end)

-- 同步检查连接状态
local connected = pg.IsConnected()   -- → boolean
```

## 异步回调 API

所有异步方法通过 `callback(err, ...)` 返回值。`err` 为空字符串表示成功，非空为错误描述。

### 查询与执行

```lua
-- 简单查询，无参数
pg.AsyncQuery("SELECT * FROM users", function(err, rows)
    if err ~= "" then log.Error(err); return end
    for _, row in ipairs(rows) do
        log.Info(row.name)
    end
end)

-- 参数化查询（$1, $2, ...）
pg.AsyncQuery("SELECT * FROM users WHERE id = $1", 42, function(err, rows)
    -- rows 为 table array，每个元素是 field_name → value 的 table
end)

-- DML 执行
pg.AsyncExecute("DELETE FROM users WHERE id = $1", 42, function(err, n)
    log.Info("deleted " .. n .. " rows")
end)

-- 无参数 DML
pg.AsyncExecute("TRUNCATE test_table", function(err, n)
    log.Info("truncated, affected: " .. n)
end)
```

**查询结果格式：**

```lua
-- rows 是一个 Lua table array（从 1 开始索引），每个元素格式：
-- {
--   { id = 1, name = "Alice", score = 95.5 },
--   { id = 2, name = "Bob",   score = 87.0 },
-- }
```

字段值类型自动转换：

| PostgreSQL 类型 | Lua 类型 |
|---|---|
| BOOL | `boolean` |
| INT2/INT4/INT8 | `number` |
| FLOAT4/FLOAT8 | `number` |
| NUMERIC/DATE/TIME/TIMESTAMP | `string` |
| TEXT/VARCHAR/UUID/JSON/INET | `string` |
| NULL | `nil` |

### 事务

```lua
pg.AsyncBegin(function(err, ok)
    if not ok then return end
    pg.AsyncExecute("INSERT INTO test VALUES ($1, $2)", 1, "hello", function(err, n)
        if err ~= "" then
            pg.AsyncRollback(function(err, ok)
                log.Info("rolled back")
            end)
        else
            pg.AsyncCommit(function(err, ok)
                log.Info("committed")
            end)
        end
    end)
end)
```

## 协程桥接 — pg.Await

允许在 Lua 协程中以**同步风格**写异步数据库代码：

```lua
local co = coroutine.create(function()
    -- 查询
    local err, rows = pg.Await("Query", "SELECT * FROM users WHERE id = $1", 42)
    if err ~= "" then log.Error(err); return end
    for _, row in ipairs(rows) do
        log.Info(row.name)
    end

    -- 插入
    local err, n = pg.Await("Execute", "INSERT INTO test VALUES ($1, $2)", 1, "hello")

    -- 事务
    local err, ok = pg.Await("Begin")
    if ok then
        pg.Await("Execute", "UPDATE users SET score = $1 WHERE id = $2", 100, 1)
        pg.Await("Commit")
    end
end)
coroutine.resume(co)
```

`pg.Await(method, ...)` 调用对应的 `pg.Async<method>`，自动处理回调与协程的 yield/resume 桥接。

**协程桥接模式：** `BridgeCallback` 辅助函数通过 `lua_xmove` 将 `sol::function` 引用从协程的 `lua_State` 迁移到 Worker 主线程的 `lua_State`，确保回调在主线程栈上执行，避免与 `coroutine.yield/resume` 冲突（详见 `register_postgresql.cpp` 的 `BridgeCallback`）。

---

# 目录结构

```
src/
  app/                   — App / ServerApp（生命周期，帧循环，关闭）
  async/                 — ShutdownManager（4阶段关闭），SignalHandler
  base/                  — Singleton, ResPath, MD5, util_string, timer_help
  log/                   — GbLog（spdlog 封装）
  timer/                 — Timer, TimerManager（SteadyTimer, SystemTimer）
  worker/                — Worker, WorkerManager, IWorkerLogic
  network/
    io/                  — Server, Client, Session, Listener, IoServicePool
    manager/             — NetworkManager（单例，统一入口）
    router/              — Router, RouteTable, MessageType
    rpc/                 — RpcCall, RpcReply, CoRpc, ThreadLocalRpcContext
    http/                — HttpServer, HttpClient, HttpSession/HttpsSession
    msgpack/             — 自定义 msgpack packer/unpacker
  script/                — Script(sol::state), protobuf↔Lua 桥接
  db/
    redis/               — Redis 连接池 + Lua 绑定（Boost.Redis）
    postgres/            — PostgreSQL 连接 + Lua 绑定（libpq）

server/                  — 正式服务器进程
  login_server/          — LoginApp (ServerApp + HttpServer)
  gateway_server/        — GatewayApp
  scene_server/          — SceneApp

test/                    — 测试进程
  server_test/           — server test (App)
  client_test/           — client test (App)
  db_test/               — 数据库集成测试（独立控制台，不依赖 App）

res/                     — 资源文件
  config/server_config.xml  — IP/Port 配置
  proto/                    — .proto 定义 + 代码生成脚本
script/                  — Lua 脚本（顶层）
  main.lua               — 入口脚本
  test.lua               — RPC 测试
  test_db.lua            — 数据库测试启动入口
  db_test_lua_redis_pg.lua— Redis + PostgreSQL 测试（含协程桥接）
  db_test_lua_test.lua   — Lua 脚本绑定基础测试
  LuaPanda.lua           — LuaPanda 调试器
protobuf/                — 生成的 .pb.h/.pb.cc
cmake/                   — 构建工具函数
3rd/                     — 第三方依赖配置
```
