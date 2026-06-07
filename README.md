# gb_server
c++ 网络游戏服务器框架

# 编译
## windows
    1. 设置profile
    2. 执行 install_deps.bat
    3. conan install . -pr=profiles/msvc_debug_pr --build=missing
    4. cmake --preset conan-debug
    5. cmake --build --preset conan-debug --parallel 或者可以用vs打开cmake文件
    注意*: vs2026环境不会默认安装到系统环境，执行以上命令需要到 Command Prompt for VS下运行


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

# 架构总览

```
                    ┌──────────────────────────────────┐
                    │         Main Thread              │
                    │  (App::Run 帧循环，管理操作)       │
                    │  系统定时器 · 事件队列 · 路由冻结    │
                    └──────────┬───────────────────────┘
                               │
          ┌────────────────────┼────────────────────┐
          ▼                    ▼                    ▼
   ┌──────────────┐   ┌──────────────┐   ┌──────────────────┐
   │ Normal Worker│   │ Normal Worker│   │  IO Thread Pool  │
   │ (线程1)       │   │ (线程2)       │   │  (N × IoWorker)  │
   │ 帧循环 + 业务  │   │ 帧循环 + 业务  │   │  TCP 网络 IO     │
   │ Lua · 定时器   │   │ Lua · 定时器   │   │  Message → Router│
   └──────┬───────┘   └──────┬───────┘   └────────┬─────────┘
          │                   │                    │
          │                   │                    ▼
          │                   │           ┌──────────────────┐
          │                   │           │     Router       │
          │                   │           │  实体路由优先     │
          │                   │           │  服务路由回退     │
          │                   │           └────────┬─────────┘
          │                   │                    │
          └────────┬──────────┘                    │
                   ▼                               │
          ┌──────────────────┐                     │
          │ ThreadPoolScheduler│                    │
          │ (N × TP 线程)     │                    │
          │ 重度计算 · 回投    │                    │
          └──────────────────┘                    │
                                                   │
          Worker 帧循环内: 轻量逻辑直接执行
                               重度计算 → ThreadPoolScheduler → 完成后回投
```

## 可执行文件

| 目标 | 入口 | 基类 | 用途 |
|---|---|---|---|---|
| `server_test` | `test/server_test/main.cpp` | `App` | 开发测试服务端 |
| `client_test` | `test/client_test/main.cpp` | `App` | 开发测试客户端 |
| `db_test` | `test/db_test/main.cpp` | 无（独立控制台） | 数据库/Redis 集成测试 |
| `unit_test` | `test/unit_test/main.cpp` | 无（独立控制台） | 单元测试（路由、msgpack、调度器等） |
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

**ServerApp** 处理 CLI 参数（`-t <type> -r <path>`），自动创建 NormalWorker、注册 Router、启动 TCP Server。

---

# 模块一：生命周期（App）

## 线程职责划分

| 线程 | 运行 | 用途 |
|---|---|---|
| **Main Thread** | `App::Run()` 帧循环 | 管理操作（系统定时器、事件队列、路由冻结），**不跑业务逻辑** |
| **Normal Worker N** | 独立线程，内部帧循环 | 按路由分配的异步消息处理，业务逻辑，Lua 脚本 |
| **Normal Worker 0** | 独立线程 | 与上述相同，无主从之分（Main Worker 概念仅用于管理） |
| **IoWorker** | `io_context::run()` | TCP 网络 IO |

## 启动流程

```
main(argc, argv)
  └─ MyApp app(argc, argv)
       └─ app.Init()
            ├─ log.Init()                           — spdlog 初始化
            ├─ WorkerManager::InitMainWorker()       — 创建 Main Worker（管理用）
            ├─ ShutdownManager::Initialize()         — 注册 4 阶段关闭回调
            ├─ SignalHandler::Initialize()           — Ctrl+C / SIGTERM
            ├─ ThreadPoolScheduler::Instance()->Init() — 预创建线程池线程
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
  │                   │    ├─ register_log()
  │                   │    ├─ register_msgpack()
  │                   │    ├─ register_proto_msg()
  │                   │    ├─ register_net()         — net.Listen/Send/BuildMeta/ParseMeta
  │                   │    ├─ RegisterRpcLua()       — net.Register/Call/RpcCall/RpcReply + rpc.Await
  │                   │    └─ register_nats()        — nats.Publish/Reply/Subscribe
  │                   ├─ require("socket.core")
  │                   ├─ start_debug.lua             — LuaPanda 调试器
  │                   └─ require("main.lua")         — ★ 用户 Lua 脚本
  ├─ NetworkManager::Freeze()                        — Lock 处理器为只读原子快照
  ├─ Router::FreezeEntityRoutes()                    — 发布实体路由快照
  └─ 帧循环（见下方）
       ├─ sys_timer_mgr_->Update()                   — 系统定时器
       ├─ ProcessMainThreadEvents()                  — Drain 主线程事件队列
       ├─ Router::FreezeEntityRoutes()               — 每帧发布路由变更
       ├─ OnUpdate(elapsed)                          — 应用层管理帧
       └─ OnTick()                                   — 应用层 Tick
```

## 帧循环

```cpp
// App::Run()
while (runding_) {
    if (SignalHandler::IsSignalReceived()) break;

    current_time = steady_clock::now();
    elapsed = current_time - last_time;
    last_time = current_time;

    // 1. 系统定时器（续期、健康检查、负载上报等）
    sys_timer_mgr_->Update();

    // 2. 处理主线程事件队列（Bind/Unbind 路由变更等）
    ProcessMainThreadEvents();

    // 3. 发布实体路由快照
    router.FreezeEntityRoutes();

    // 4. 应用层管理帧（不再包含业务逻辑）
    OnUpdate(elapsed.count());
    OnTick();

    // 5. 帧率控制（默认 60fps ≈ 16ms）
    frame_time = now - current_time;
    if (frame_time < frame_duration_)
        sleep_for(frame_duration_ - frame_time);
}
```

**关键变化：** 主线程不再调用 `main_worker->ProcessFrame()`，业务逻辑全部在 Normal Worker 独立线程中处理。Main Worker 仅作为管理操作的容器（主线程事件队列）。

## Normal Worker 帧循环

```cpp
// Worker::Run() — 每个 Normal Worker 的独立线程
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

// Worker::ProcessFrame(float elapsed)
void Worker::ProcessFrame(float elapsed) {
    // 1. 派发到期的定时器
    timer_manager_->Update();

    // 2. 执行队列中所有待处理任务
    while (events_.try_dequeue(func)) {
        func();
    }

    // 3. 调用用户逻辑
    if (worker_logic_)
        worker_logic_->OnUpdate(elapsed);

    // 4. Tick
    worker_logic_->OnTick();
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
            │    ├─ 主线程 sys_timer_mgr_ 关闭
            │    ├─ 所有 Normal Worker TimerManager EnterShutdownMode
            │    └─ 执行最后一次定时器 Update + 事件处理
            │
            ├─ Phase 3: ProcessingTasks
            │    ├─ ★ 先 Drain ThreadPool —— 重度任务完成后回调投递到 Worker 队列
            │    ├─ 所有 Normal Worker EnterShutdownMode
            │    ├─ 处理主线程剩余事件
            │    └─ 等待 Normal Worker 队列为空（最长 5s）
            │
            └─ Phase 4: Cleaning
                 ├─ App::OnCleanup()
                 ├─ 关闭 Redis 连接池
                 ├─ 每个 Normal Worker CleanupInWorkerThread(5s)
                 │   超时则直接 Stop()
                 ├─ join 所有 worker 线程
                 ├─ 主 Worker::OnCleanup()
                 ├─ App::OnUnInit()
                 └─ SignalHandler::Cleanup()
```

**关闭顺序关键：** ThreadPool 先于 Worker drain，确保 ThreadPool 回调已投递到 Worker 队列后才进入 Worker 关闭流程。

---

# 模块二：线程模型 & 调度系统

## 线程模型

```
Main Thread (App::Run)
┌──────────────────────────────────────────────────┐
│ 管理帧循环                                        │
│ - 系统定时器 Update                               │
│ - 主线程事件队列 Drain                            │
│ - FreezeEntityRoutes (发布路由快照)                │
│ - OnUpdate / OnTick (管理逻辑)                    │
│ 不跑业务 Worker 帧                                │
└──────────────────────────────────────────────────┘

Normal Worker 0 .. N (独立线程)
┌──────────────────────────────────────────────────┐
│ while (runing_):                                  │
│   wait_for(50ms) / 新任务唤醒                     │
│   ProcessFrame(elapsed)                           │
│   ├─ TimerManager::Update()                       │
│   ├─ events_ queue drain                          │
│   ├─ WorkerLogic::OnUpdate                        │
│   └─ WorkerLogic::OnTick                          │
└──────────────────────────────────────────────────┘

IoWorker 0 .. N (独立线程)
┌──────────────────────────────────────────────────┐
│ io_context::run()                                 │
│ Accept / Read / Write / SSL / Heartbeat           │
│ 完整消息 → OnReceiveCall → Dispatch → Router      │
│ → 投递到目标 Worker 队列                          │
└──────────────────────────────────────────────────┘

ThreadPool (N 个专用线程)
┌──────────────────────────────────────────────────┐
│ 抢占式任务执行                                     │
│ - 寻路 / AI 批量 / 大包序列化 / 数据库等重度计算   │
│ - 执行完后自动回投 Worker 线程                     │
│ - 通过 ThreadPoolScheduler 访问                   │
└──────────────────────────────────────────────────┘
```

## ThreadPoolScheduler — 抢占式线程池调度

重度任务（寻路、AI 批量计算、大包序列化）不应阻塞 Worker 帧循环。`ThreadPoolScheduler` 提供统一的调度入口：

```cpp
#include "async/thread_pool_scheduler.h"

// ─── 回调风格 ───
ThreadPoolScheduler::Instance()->Dispatch<PathResult>(
    []() -> PathResult {
        return Pathfind(start, end);  // 在 TP 线程执行
    },
    [](PathResult r) {
        HandleResult(r);  // 回到发起 Worker 线程
    }
);

// ─── 执行+回投（不关心返回值） ───
ThreadPoolScheduler::Instance()->Post([]() {
    HeavyComputation();  // TP 线程执行，完成后回投
});

// ─── 协程风格 ───
async_simple::coro::Lazy<void> MyCoroutine() {
    auto result = co_await ThreadPoolScheduler::Instance()->Schedule<PathResult>(
        []() { return Pathfind(start, end); }
    );
    HandleResult(result);  // 回到 Worker 线程
}
```

**调度器 API：**

| 方法 | 返回值 | 说明 |
|---|---|---|
| `Dispatch<T>(task, cb)` | `void` | TP 执行 task，完成后回投 Worker 调用 cb |
| `Post(task)` | `void` | TP 执行 task + 回投 Worker（无返回值） |
| `Schedule<T>(task)` | `ThreadPoolAwaiter<T>` | 协程风格，`co_await` 后自动回 Worker |
| `Execute(task)` | `void` | 裸执行，不回投（底层引擎） |

## 任务投递

```cpp
// 任何线程向指定 Worker 投递任务
worker->Post([this]() {
    // 在 Worker 线程上下文中执行
});

// 通过 Router 获取 WorkerExecutor
// 统一入口：根据 SetPolicy 选择 Stateful 或 Stateless 策略
router.SetPolicy(Router::Policy::Stateful);   // Scene Server：entity_id 精确绑定
router.SetPolicy(Router::Policy::Stateless); // Gateway Server：hash 路由

auto executor = router.GetExecutor(/*type=*/0, entity_id);
if (executor.HasWorker()) {
    executor.Dispatch([]() {
        // 自动路由到对应 Worker
    });
}
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
    uint64_t     entity_id{0};      // 路由主键（玩家 ID / 场景 ID / NPC ID），完整 64 位
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
NetworkManager::Instance()->Send(session, type, entity_id, protoMsg);

// Lua 发送 protobuf 消息
net.Send(session, type, entity_id, "ProtoName", lua_msg)
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
                      ├─ MsgMode::Msg → 查 ListenFunction
                      │       → GetExecutor(type, entity_id)
                      │          ├─ Stateful：entity_id 精确绑定，未命中则回退 hash
                      │          └─ Stateless：纯 hash
                      │
                      ├─ MsgMode::Request → 查 RpcFunction
                      │       → GetExecutor(type, entity_id)
                      │          ├─ Stateful：entity_id 精确绑定，未命中则回退 hash
                      │          └─ Stateless：纯 hash
                      │
                      └─ MsgMode::Response → 从 sequence 解码 WorkerIndex
                                              → Worker::Post → TakePendingRpc
                                              → RpcCall::Done → 触发回调
```

---

# 模块四：路由机制

## 路由总览

gb_server 有两种核心路由机制 + HTTP 独立路由：

| 路由类型 | 路由依据 | 路由目标 | 调度方式 |
|---|---|---|---|
| **统一路由** | `Router::Policy` 决定 | Stateful：entity_id 精确绑定；Stateless：hash 路由 | `Router::GetExecutor(type, entity_id)` |
| **RPC 响应** | `meta.sequence` (WorkerIndex) | 解码直投指定 Worker | 不经过 Router |
| **HTTP 请求** | URL path + HTTP method | 匹配 RouteEntry handler | 线性遍历 routes vector |

## Router 策略路由（统一入口）

`Router` 通过 `Policy` 选择路由策略，两类服务器各取所需：

```
┌─ Policy::Stateful（Scene Server） ────────────────────────────────────┐
│  meta.entity_id != 0                                                 │
│       │                                                              │
│       ▼                                                              │
│  LockFreeRouteTable::Lookup(entity_id)                              │
│       │  双缓冲 + 排序向量 + 二分查找，读端无锁                        │
│       ▼                                                              │
│  命中 → WorkerExecutor::Dispatch → Worker::Post                      │
│       │                                                              │
│       └─ 未命中 → 回退到 hash 路由（新建 entity 时）                   │
└──────────────────────────────────────────────────────────────────────┘

┌─ Policy::Stateless（Gateway Server） ─────────────────────────────────┐
│  meta.entity_id 作为 route_id                                         │
│       │                                                              │
│       ▼                                                              │
│  ServiceWorkerType 分类 → workers[hash(route_id) % N]                │
│       │                                                              │
│       ▼                                                              │
│  WorkerExecutor::Dispatch → Worker::Post                              │
└──────────────────────────────────────────────────────────────────────┘
```

```
┌─ 写端（主线程独占） ──────────────────────────┐
│  pending_ 向量                                  │
│  Bind(entity_begin, entity_end, worker_index)   │
│    → 处理区间重叠/拆分 → 追加条目               │
│  Unbind(entity_id)                              │
│    → 移除/拆分包含该 entity 的区间              │
│  Freeze()                                       │
│    → 排序 → 创建只读快照 → atomic swap → 回收   │
└────────────────────────────────────────────────┘
                      │ 每帧 FreezeEntityRoutes()
                      ▼
┌─ 读端（任意线程，无锁） ──────────────────────────┐
│  atomic_load → 二分查找 entity_id                 │
│  → 返回 worker_index 或 kInvalidWorker            │
└────────────────────────────────────────────────┘
```

```cpp
// 统一 API
router.SetPolicy(Router::Policy::Stateful);   // Scene Server
router.SetPolicy(Router::Policy::Stateless);  // Gateway Server（默认）

router.BindEntity(entity_begin, entity_end, worker_index);  // 仅 Stateful
router.UnbindEntity(entity_id);                              // 仅 Stateful

// 查找：统一入口，内部根据 Policy 自动走对应路径
auto executor = router.GetExecutor(/*type=*/0, entity_id);
if (executor.HasWorker()) {
    executor.Dispatch([]() { /* 直投对应 Worker */ });
}
```

### Stateful 与 Stateless 的配合

```
NetworkManager::Dispatch()
  │
  ├─ Msg / Request 模式:
  │       → GetExecutor(type, entity_id)
  │          ├─ Stateful：  精确绑定，未命中则回退 hash
  │          └─ Stateless： 纯 hash
  │
  └─ Response 模式:
      → 从 sequence 解码 WorkerIndex，直投（不经过 Router）
```

## ServiceWorkerType 分类（两种策略共享）

ServiceWorkerType 将 Worker 按职责分组，Stateless 路由和 Stateful 回退都依赖它：

```
  meta.type → ResolveServiceWorkerType(MessageType)
       │
       ▼
  ServiceWorkerType → workers[type] → Worker vector
       │
       ▼
  PickWorker(workers, route_id)
       │  默认: route_id % workers.size()
       │  可自定义: SetWorkerIndexSelector(...)
       ▼
  WorkerExecutor::Dispatch(func) → Worker::Post → Worker::ProcessFrame
```

**ServiceWorkerType：**

| 枚举值 | 名称 | 默认映射 |
|---|---|---|
| `SWT_Normal = 0` | 普通业务 | 所有未显式映射的消息 |
| `SWT_AI = 1` | AI 业务 | 自定义 resolver 映射 |
| `SWT_Navigation = 2` | 寻路业务 | 自定义 resolver 映射 |

**自定义路由策略：**

```cpp
// 自定义消息类型 → ServiceWorkerType 映射
router.SetServiceTypeResolver([](MessageType type) -> ServiceWorkerType {
    if (type >= 10000 && type < 20000) return SWT_AI;
    return SWT_Normal;
});

// 自定义 Worker 选择
router.SetWorkerIndexSelector([](auto& workers, auto type, auto route_id) {
    return route_id % workers.size();
});
```

## RPC 响应路由（零查找直投）

RPC 响应路由从 `meta.sequence` 中解码目标 Worker，不经过 Router：

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

| 特性 | 实体路由 | 服务路由 | HTTP |
|---|---|---|---|
| 路由依据 | `entity_id` (uint64) | `type` → `ServiceWorkerType` | URL path + method |
| 路由表 | `LockFreeRouteTable`（双缓冲+二分查找） | `RouteTable`（`array<Worker vector>`） | `vector<RouteEntry>` |
| 锁 | 读端无锁 / 写端主线程独占 | `mutex` 保护 | mutex+快照遍历 |
| 线程模型 | IO → Worker 队列 | IO → Worker 队列 | IO 线程直接执行 |
| 序列化 | protobuf | protobuf / msgpack | JSON/文本/任意 |

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
call->SetCallBack([](RpcErrorCode err, int result) {
    // 处理响应，err == RpcErrorCode::None 表示成功
});
call->SetTimeout([]() {
    // 超时处理
}, 5000);
call->SetSession(session);                  // 绑定会话（可选）
NetworkManager::Instance()->Call(call, "method_name", 0, arg1, arg2);

// ─── 协程风格（CoRpc）───
// CoRpc<T>::execute(call, method, id, args...) → async_simple::Lazy<RpcResult<T>>
auto result = co_await CoRpc<int>::execute(
    std::make_shared<RpcCall>(), "method_name", 0, arg1, arg2);
// result.error_code 判断成功，result.value 获取返回值

// 多返回值协程
auto [a, b, c] = co_await CoRpc<int, std::string, float>::execute(
    std::make_shared<RpcCall>(), "multi_return", 0, arg1);

// 无返回值协程
co_await CoRpc<void>::execute(
    std::make_shared<RpcCall>(), "void_method");
```

## 完整 RPC 流程

```
┌─ 调用端 (Worker 线程) ─────────────────────────────────┐
│                                                         │
│  NetworkManager::Call(call, "method", 0, args...)         │
│    0. meta.entity_id = id                                 │
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
│    2. GetExecutor(meta.type, meta.entity_id)             │
│       ├─ Stateful：entity_id 精确绑定，未命中回退 hash   │
│       └─ Stateless：纯 hash                              │
│    3. Worker 执行处理器, reply.Invoke(data)              │
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
|---|---|---|
| `RpcCall` | RPC 请求对象，管理超时/取消/回调/状态 |
| `RpcReply` | RPC 响应对象，服务端通过它 `Invoke()` 返回数据 |
| `CoRpc<T>` | 协程包装器，返回 `RpcResult<T>`（错误码 + 值） |
| `RpcErrorCode` | 统一错误码枚举（None/Timeout/Cancel/RemoteError 等） |
| `RpcResult<T>` | 统一返回值，`.error_code` + `.value`，支持 `co_await` |
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
       │    ├─ register_net()              — net.Listen/Send/BuildMeta/ParseMeta
        │    ├─ RegisterRpcLua()            — net.Register/Call/RpcCall/RpcReply + rpc.Await
       │    └─ register_nats()             — nats.Publish/Reply/Subscribe
       ├─ require("socket.core")           — LuaSocket
       ├─ start_debug.lua                  — LuaPanda 调试器（可选）
       └─ require("main.lua")              — ★ 用户 Lua 脚本
            ├─ net.Listen(type, func, "ProtoName")
            ├─ net.Register("method", func)
            ├─ nats.Connect/Publish/Reply/Subscribe
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
call:SetCallBack(function(reply, err, result)
    log.Info("RPC response: " .. result)
end)
net.Call(call, "lua_rpc_test_args", 0, "hello")
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

### net 表（内置消息 + RPC）

| API | 说明 |
|---|---|
| `net.Listen(type, func, "ProtoName")` | 注册消息处理器，type 匹配 MessageType，第三个参数指定 protobuf 类型名 |
| `net.Register("method", func)` | 注册 RPC 方法，func(reply, args...) |
| `net.Send(session, type, entity_id, "ProtoName", msg)` | 发送 protobuf 消息 |
| `net.Call(call, "method", id, ...)` | 发起 RPC 调用（id 为 entity_id 路由参数） |
| `rpc.Await("method", id, setup, ...)` | 协程风格 RPC 调用，`coroutine.create` 中使用，返回 `(err, ...)`；`setup` 为可选 `function(call)`，可在发送前配置 RpcCall（SetSession、SetTimeout 等） |
| `net.BuildMeta(meta_table)` | 将 Lua meta table → 二进制 bytes（用于 NATS） |
| `net.ParseMeta(meta_bytes)` | 二进制 bytes → Lua meta table |
| `log.Info(str)` / `log.Error(str)` / `log.Warning(str)` | 日志（带源码位置追踪） |
| `msgpack.pack(...)` | msgpack 序列化 |
| `msgpack.unpack(data)` | msgpack 反序列化 |
| `create_msg("ProtoName")` | 创建 protobuf 消息对象 |

### RpcCall / RpcReply 对象

| API | 说明 |
|---|---|
| `RpcCall.new()` | 创建 RPC 调用对象 |
| `RpcCall:SetSession(session)` | 绑定会话 |
| `RpcCall:SetCallBack(func)` | 设置回调，`func(reply, err, 返回值...)`，`err` 为 `RpcErrorCode` |
| `RpcCall:SetTimeout(func, ms)` | 设置超时回调 |
| `RpcCall:SetId(id)` | 设置 entity_id |
| `RpcCall:Cancel()` | 取消 RPC 调用 |
| `RpcReply:Invoke(...)` | 在 RPC 处理器中返回响应 |

### nats 表（NATS 消息系统）

| API | 说明 |
|---|---|
| `nats.Connect(url)` | 连接 NATS Server |
| `nats.Disconnect()` | 断开连接 |
| `nats.IsConnected()` | 检查连接状态 → bool |
| `nats.Publish(subject, meta_bytes, data)` | 发送消息（data 为 string，raw bytes） |
| `nats.Publish(subject, meta_bytes, proto_msg)` | 发送 protobuf 消息 |
| `nats.Publish(subject, meta_bytes, ...)` | 发送 msgpack 变参 |
| `nats.Reply(reply_to, meta_bytes, data)` | 回复 Request（raw bytes） |
| `nats.Reply(reply_to, meta_bytes, proto_msg)` | 回复 Request（protobuf） |
| `nats.Reply(reply_to, meta_bytes, ...)` | 回复 Request（msgpack 变参）|
| `nats.Subscribe(subject, handler)` | 订阅 raw bytes，`handler(meta_tbl, body_str, reply_to)` |
| `nats.Subscribe(subject, handler, "ProtoName")` | 订阅 protobuf 消息，`handler(meta_tbl, proto_msg, reply_to)` |
| `nats.Subscribe(subject, handler, "msgpack")` | 订阅 msgpack 消息，`handler(meta_tbl, values_tbl, reply_to)` |

## Lua 注意事项

1. **每个 Worker 独立 Lua 状态**：`net.Listen` 和 `net.Register` 在每个 Worker 线程各调用一次
2. **注册必须在 Freeze 之前**：所有 Lua 初始化完成后才会调用 `Freeze()`，在此之前必须完成所有注册
3. **Lua 的 protobuf 桥接**：protobuf 消息对象直接传递给 Lua（非 table），通过 `create_msg` 创建、sol2 usertype 访问字段。`protobuf_new_table`（protobuf → Lua table 转换）已废弃，仅 `lua_pb_parse.{h,cpp}` 保留向后兼容。
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

# 模块十二：NATS 消息系统

基于 **nats.c**（C 客户端库），向 Lua 暴露统一消息通信接口（Publish/Reply/Request/Subscribe）。集成 `Meta` 消息头，支持 raw bytes、protobuf、msgpack 三种载荷格式。

## 架构概览

```
Lua Worker Thread                      nats.c I/O Threads     NATS Server
      │                                      │                    │
      ├─ nats.Publish(subject, meta, ...) ───┤                    │
      ├─ nats.Subscribe(subject, handler) ───┤                    │
      │                                      ├─ natsConnection    │
      │                                      │   ··· async ···    │
      │                                      ├─ OnNatsSubMsg() ───┤
      │  Post 到订阅 Worker 队列 ←───────────┤                    │
      ▼                                      ▼                    ▼
  Worker 帧循环                          nats.c 线程池        NATS Server
```

- 全局唯一 NATS 连接（`natsManager` 单例）
- `nats.c` 的回调在 nats 内部 I/O 线程上触发，通过 `Worker::Post` 投递到订阅的 Worker 线程
- `subscribe` 时记录当前 `worker_index`，回调直接投递到该 Worker 的帧循环
- 所有 Lua handler 在 Worker 线程上下文中安全执行

## C++ API 参考

### 连接

```cpp
#include "network/nats/nats_manager.h"

NatsManager::Instance()->Connect("nats://127.0.0.1:4222");
NatsManager::Instance()->Disconnect();
bool ok = NatsManager::Instance()->IsConnected();
```

### Publish

```cpp
// raw bytes
NatsManager::Instance()->Publish("subject", meta, data_bytes);

// protobuf
NatsManager::Instance()->Publish("subject", meta, protoMsg);

// msgpack 变参
NatsManager::Instance()->Publish("subject", meta, arg1, arg2, arg3);
```

### Subscribe

```cpp
// raw bytes handler
NatsManager::Instance()->Subscribe("subject",
    [](const Meta& m, const std::vector<uint8_t>& body,
       const std::string& reply_to) {});

// C++ 模板 Subscribe（自动反序列化）
NatsManager::Instance()->Subscribe<MyProto>("subject",
    [](NatsResult<MyProto> r) { /* r.value 是 MyProto */ });

NatsManager::Instance()->Subscribe<int>("subject",
    [](NatsResult<int> r) { /* r.value 是 int */ });

NatsManager::Instance()->Subscribe<int, float>("subject",
    [](NatsResult<int, float> r) {
        auto [a, b] = r.value;  // tuple 解包
    });
```

### Request（协程风格）

```cpp
// 原始 bytes
auto raw = co_await NatsManager::Instance()->RequestRaw(
    "subject", meta, data_bytes, 5s);

// 模板 Request（protobuf 请求 + protobuf 响应）
auto r = co_await NatsManager::Instance()->Request<MyProto>(
    "subject", meta, request_msg);
// r.value 是 MyProto

// 模板 Request（msgpack 请求 + msgpack 响应）
auto r = co_await NatsManager::Instance()->Request<int>(
    "subject", meta, arg1, arg2);
```

### Reply

```cpp
// raw bytes
NatsManager::Instance()->Reply(reply_to, meta, data_bytes);

// protobuf
NatsManager::Instance()->Reply(reply_to, meta, protoMsg);

// msgpack 变参
NatsManager::Instance()->Reply(reply_to, meta, arg1, arg2);
```

## Lua API

NATS Lua API 统一在 `nats` 表下（`script/register_nats.cpp` 注册），通过 `sol::overload` 实现多态：

| Lua API | 说明 |
|---|---|
| `nats.Connect(url)` | 连接 NATS Server |
| `nats.Disconnect()` | 断开连接 |
| `nats.IsConnected()` | 检查连接状态 |
| `nats.Publish(subject, meta, data)` | 发送 raw bytes 消息 |
| `nats.Publish(subject, meta, proto_msg)` | 发送 protobuf 消息 |
| `nats.Publish(subject, meta, ...)` | 发送 msgpack 变参 |
| `nats.Reply(reply_to, meta, data)` | 回复 raw bytes |
| `nats.Reply(reply_to, meta, proto_msg)` | 回复 protobuf |
| `nats.Reply(reply_to, meta, ...)` | 回复 msgpack 变参 |
| `nats.Subscribe(subject, handler)` | 订阅 raw bytes，handler(meta, body_str, reply_to) |
| `nats.Subscribe(subject, handler, "ProtoName")` | 订阅 protobuf，handler(meta, proto_msg, reply_to) |
| `nats.Subscribe(subject, handler, "msgpack")` | 订阅 msgpack，handler(meta, values_tbl, reply_to) |

**NATS Publish/Reply 3 种载荷格式：**

| 第三个参数类型 | 序列化方式 |
|---|---|
| `string` | 原始 bytes 发送 |
| protobuf userdata（`create_msg` 创建） | `Message::SerializeToString` |
| 无第三个参数，使用 `...` 变参 | msgpack 打包 |

**NATS Subscribe 调用约定：**

| 参数模式 | handler 签名 |
|---|---|
| `Subscribe(subject, handler)` 2 参数 | `function(meta_tbl, body_str, reply_to)` |
| `Subscribe(subject, handler, "ProtoName")` 3 参数 | `function(meta_tbl, proto_msg, reply_to)` |
| `Subscribe(subject, handler, "msgpack")` 3 参数 | `function(meta_tbl, values_tbl, reply_to)` |

## 关键实现细节

- **`NatsResult<T...>`** — 模板结果类型，单值 `T .value` / 多值 `tuple<T...> .value` / `void` 仅状态
- **`NatsError`** — 错误码：`OK(0)`, `Timeout(-1)`, `Disconnected(-2)`, `PublishFailed(-3)`, `SubscribeFailed(-4)`, `RequestFailed(-5)`
- **`NatsHandler`** — `std::function<void(const Meta&, const std::vector<uint8_t>&, const std::string&)>`，`reply_to` 在 Request 消息时不为空
- **`BuildMeta`/`ParseMeta`** — 在 `net` 表下（`register_script.cpp`），将 Lua table ↔ 二进制 Meta bytes 互相转换，供 NATS Lua 调用者使用
- 注册表文件：[`src/network/nats/register_nats.cpp`](src/network/nats/register_nats.cpp) 同时负责 C++ 和 Lua 绑定
- NATS Subscribe proto handler 直接传递 protobuf 消息对象给 Lua（非 table 转换）—— 与 `net.Listen` 行为一致

---

# 目录结构

```
src/
  app/                   — App / ServerApp（生命周期，帧循环，关闭）
  async/                 — ShutdownManager, SignalHandler,
                           ThreadPool, ThreadPoolScheduler
  base/                  — Singleton, ResPath, MD5, util_string, timer_help
  log/                   — GbLog（spdlog 封装）
  timer/                 — Timer, TimerManager（SteadyTimer, SystemTimer）
  worker/                — Worker, WorkerManager, IWorkerLogic
  network/
    io/                  — Server, Client, Session, Listener, IoServicePool
    manager/             — NetworkManager（单例，统一入口）
    router/              — Router, RouteTable, LockFreeRouteTable, MessageType
    rpc/                 — RpcCall, RpcReply, CoRpc, register_rpc,
                           ThreadLocalRpcContext
    nats/                — NatsManager, register_nats（Lua API）
    http/                — HttpServer, HttpClient, HttpSession/HttpsSession
    msgpack/             — 自定义 msgpack packer/unpacker
  script/                — Script(sol::state), protobuf↔Lua 桥接,
                           register_script
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
  unit_test/             — 单元测试（app_test, msgpack_test, route_test, scheduler_test）

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
