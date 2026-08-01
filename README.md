# gb_server
c++ 网络游戏服务器框架

# 编译
## windows
    注意*: vs2026环境不会默认安装到系统环境，执行以上命令需要到 x64 Native Tools Command Prompt for VS下运行
    1. 设置profile (主要设置msvc工具链的路径，需要根据情况修改)
    2. conan install . -pr=profiles/msvc_debug_pr --build=missing
    3. cmake --preset conan-debug
    4. cmake --build --preset conan-debug --parallel 或者可以用vs打开cmake文件
    


## linux
    注意*: linux下默认python环境可能不让安装conan,需要建一个虚拟环境，之后每次操作都是在这个虚拟环境下进行
    1. 设置profile,添加下面两行 conan有个bug Conan + Boost 1.90 的 recipe 在初始化阶段就不兼容 cobalt 模块，
        取消boost charconv模块的float128支持 （这里只是说明下预设profile文件已经设置好了，这一步可以跳过）
        [options]
        boost/*:without_cobalt=True
        [conf]
        tools.build:cxxflags+=["-DBOOST_CHARCONV_DISABLE_FLOAT128"]
    2. conan install . -pr=profiles/clang_debug_pr --build=missing
    3. cmake --preset conan-debug
    4. cmake --build --preset conan-debug --parallel

---

# 架构总览

```
                    ┌──────────────────────────────────┐
                    │         Main Thread              │
                    │  (App::Run 帧循环)                │
                    │  系统消息(entity_id==0) · 路由冻结  │
                    │  main_worker 帧循环 · 管理逻辑     │
                    └──────────┬───────────────────────┘
                               │  entity_id == 0 的消息
          ┌────────────────────┼────────────────────┐
          ▼                    ▼                    ▼
   ┌──────────────┐   ┌──────────────┐   ┌──────────────────┐
   │ Normal Worker│   │ Normal Worker│   │  IO Thread Pool  │
   │ (线程1)       │   │ (线程2)       │   │  (N × IoWorker)  │
   │ 帧循环 + 业务  │   │ 帧循环 + 业务  │   │  TCP 收包/拆包    │
   │ Lua · 定时器   │   │ Lua · 定时器   │   │  MessageStream   │
   └──────┬───────┘   └──────┬───────┘   └────────┬─────────┘
          │                   │                    │
          │                   │                    ▼
          │                   │           ┌──────────────────┐
          │                   │           │     Router       │
          │                   │           │  uid==0 → 主线程  │
          │                   │           │  Stateful → 实体  │
          │                   │           │  Stateless → hash │
          │                   │           └────────┬─────────┘
          │                   │                    │
           └────────┬──────────┘
                    ▼
           ┌──────────────────┐
           │ ThreadPoolScheduler│
           │ (N × TP 线程)     │
           │ 重度计算 · 回投    │
           └──────────────────┘

           Worker 帧循环内: 轻量逻辑直接执行
                                重度计算 → ThreadPoolScheduler → 完成后回投
```

## 可执行文件

| 目标 | 入口 | 基类 | 用途 |
|---|---|---|---|---|
| `server_test` | `test/server_test/main.cpp` | `App` | 开发测试服务端 |
| `client_test` | `test/client_test/main.cpp` | `App` | 开发测试客户端（RPC 协程调用等） |
| `db_test` | `test/db_test/main.cpp` | 无（独立控制台） | 数据库/Redis 集成测试 |
| `unit_test` | `test/unit_test/main.cpp` | 无（Catch2） | 单元测试（msgpack / 路由 / 调度器 / 定时器 / App） |
| `login_server` | `server/login_server/main.cpp` | `ServerApp` | 登录服务器（带 HttpServer） |
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

**ServerApp** 处理 CLI 参数（`-t <type> -r <path>`），自动创建 NormalWorker、注册 Router、启动 TCP Server。`-t` 对应 `APP_TYPE` 位标志值（login=8, scene=32, gateway=64），`-r` 设置资源根路径（默认 `./res`）。

---

# 模块一：生命周期（App）

## 线程职责划分

| 线程 | 运行 | 用途 |
|---|---|---|
| **Main Thread** | `App::Run()` 帧循环 | 管理操作（系统定时器、实体路由冻结、OnUpdate/OnTick），并驱动 **main_worker 帧循环**（处理 entity_id=0 的系统消息） |
| **Normal Worker N** | 独立线程，内部帧循环 | 按路由分配的异步消息处理，业务逻辑，Lua 脚本 |
| **IoWorker** | `io_context::run()` | TCP 网络 IO（收包、拆包、分发到 Router） |
| **ThreadPool** | 专用线程池 | 重度计算（寻路、批量计算等），执行后回投 Worker |

## 启动流程

```
main(argc, argv)
  └─ MyApp app(argc, argv)
       └─ app.Init()
            ├─ log.Init()                           — spdlog 初始化（ResPath 定位 log4/test.log）
            ├─ WorkerManager::InitMainWorker()       — 创建 Main Worker（管理 + 系统消息）
            ├─ ShutdownManager::Initialize()         — 注册 4 阶段关闭回调
            ├─ SignalHandler::Initialize()           — Ctrl+C / SIGTERM
            ├─ ThreadPoolScheduler::Instance()->Init() — 预创建线程池线程
            ├─ OnInit()                              — ★ 虚函数，应用层初始化
            │    ├─ [ServerApp] 解析 -t -r CLI 参数
            │    ├─ [ServerApp] 创建 NormalWorker + 注册 Router
            │    ├─ [ServerApp] 创建 Server、启动 TCP
            │    └─ OnServerInit()                   — ★ 子类钩子（注册 Listen/Register、启动 HTTP 等）
            └─ runding_ = true

app.Run()
  ├─ OnStartup()                                     — ★ 启动所有 Worker
  │    └─ ServerApp::OnStartup()
  │         └─ worker->OnStartup() → Worker::OnStart() → InitLua()
  │              ├─ open_libraries()                 — Lua 标准库
  │              ├─ _lua_(scriptPtr)                 — 注册 C++ 绑定
  │              │    ├─ register_log()              — log.Info/Error/Warning（带源码位置）
  │              │    ├─ register_msgpack()          — msgpack.pack/unpack
  │              │    ├─ register_proto_msg()        — protobuf 消息类型注册
  │              │    ├─ register_net()              — net.Listen/UnListen/Send/BuildMeta/ParseMeta + Meta/枚举
  │              │    ├─ RegisterRpcLua()            — net.Register/Call + RpcCall/RpcReply + rpc.Await
  │              │    ├─ register_redis()            — redis.*（连接池 + 异步 + Await）
  │              │    ├─ register_postgresql()       — pg.*（异步 + Await）
  │              │    ├─ register_nats()             — nats.Publish/Reply/Subscribe/AsyncRequest
  │              │    ├─ register_etcd()             — etcd.*（KV + Watch + Await）
  │              │    └─ register_timer()            — timer.Register/RegisterSystem/UnRegister
  │              ├─ require("socket.core")           — LuaSocket
  │              ├─ start_debug.lua                  — LuaPanda 调试器（可选，127.0.0.1:8828）
  │              └─ require("main.lua")              — ★ 用户 Lua 脚本
  ├─ NetworkManager::Freeze()                        — 处理器映射冻结为只读原子快照
  ├─ Router::Freeze()                                — RouteTable + 路由策略冻结
  └─ 帧循环（见下方）
       ├─ Router::FreezeEntityRoutes()               — 发布实体路由快照
       ├─ OnUpdate(elapsed)                          — 应用层管理帧
       ├─ OnTick()                                   — 应用层 Tick
       └─ main_worker->ProcessFrame()                — 系统消息 + 主线程定时器
```

## 帧循环

```cpp
// App::Run()
while (runding_) {
    if (gb::SignalHandler::IsSignalReceived()) break;

    // 1. 冻结实体路由表（发布路由快照，双缓冲 swap）
    gb::NetworkManager::Instance()->GetRouter().FreezeEntityRoutes();

    // 2. 主线程管理帧（由子类实现）
    if (OnUpdate(elapsed.count()) != 0) break;
    if (OnTick() != 0) break;

    // 3. 主线程 Worker 帧循环（entity_id=0 的系统消息 + 主线程定时器）
    main_worker->ProcessFrame(elapsed.count());

    // 4. 帧率控制（默认 60fps ≈ 16ms）
    frame_time = now - current_time;
    if (frame_time < frame_duration_)
        sleep_for(frame_duration_ - frame_time);
}
```

**要点：** 主线程不再处理业务逻辑，但**会**驱动 main_worker 的帧循环——`user_unique_id == 0` 的系统消息（如 etcd 相关）和主线程定时器在 Main Thread 上执行。业务消息全部在 Normal Worker 独立线程中处理。

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
    // 1. 派发到期的定时器（Steady + System 双队列）
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
            │        → 所有 IoWorker 停止 accept/read/write，完成待处理 IO
            │
            ├─ Phase 2: CompletingTimers
            │    ├─ 主线程 + 所有 Normal Worker 的 TimerManager EnterShutdownMode
            │    └─ 执行最后一次 ProcessFrame（定时器回调 + 剩余任务）
            │
            ├─ Phase 3: ProcessingTasks
            │    ├─ ★ 先 Drain ThreadPool —— 重度任务完成后回调投递到 Worker 队列
            │    ├─ 所有 Worker EnterShutdownMode（不再接受新任务）
            │    └─ 等待所有 Worker 待处理任务清空（最长 5s）
            │
            └─ Phase 4: Cleaning
                 ├─ App::OnCleanup()
                 ├─ ThreadPoolScheduler::Stop() + 关闭 Redis 连接池
                 ├─ 每个 Normal Worker CleanupInWorkerThread(5s)，超时强制 Stop()
                 ├─ join 所有 worker 线程
                 ├─ main_worker->OnCleanup()
                 ├─ App::OnUnInit()
                 └─ SignalHandler::Cleanup()
```

**关闭顺序关键：** ThreadPool 先于 Worker drain，确保 ThreadPool 回调已投递到 Worker 队列后才进入 Worker 关闭流程。

---

# 模块二：线程模型 & 调度系统

## Worker（src/worker/worker.h）

每个 Normal Worker 拥有：独立线程 + 帧循环、自己的 `TimerManager`、自己的 Lua 状态（sol::state）、消息队列（moodycamel concurrentqueue）、RPC 序列号分配与待处理映射。

| API | 说明 |
|---|---|
| `Post(std::function<void()>)` | 投递任务到该 Worker 队列（线程安全，任意线程可调用） |
| `ProcessFrame(elapsed)` | 帧循环：定时器更新 → 队列任务 → OnUpdate → OnTick |
| `OnStart() / OnStop()` | 线程启动/停止钩子（OnStart 里 InitLua） |
| `StorePendingRpc(local_seq, call)` / `TakePendingRpc(local_seq)` | RPC 待处理映射（成员变量，从 thread_local 迁移，为线程池做准备） |
| `GetPendingTaskCount()` | 队列中待处理任务数 |
| `CleanupInWorkerThread(timeout)` | 关闭时在 Worker 线程内执行清理（超时强制 Stop） |

## WorkerManager（src/worker/worker_manager.h）

| API | 说明 |
|---|---|
| `InitMainWorker()` | 创建 Main Worker（管理逻辑 + entity_id=0 系统消息） |
| `CreateWorker(logic, service_type)` | 创建 Normal Worker（`IWorkerLogic` 接口 + `ServiceWorkerType`） |
| `GetWorker(index)` / `GetMainWorker()` | 获取 Worker 指针 |
| `PostToMain(func)` | 投递到主线程 Worker |
| `BroadcastToWorkers(func)` | 广播到所有 Normal Worker |
| `IsMainThread()` | 全局主线程判断 |

## ThreadPoolScheduler（src/async/thread_pool_scheduler.h）

**重度计算任务专用的固定线程池**，单例 + `async_simple::Executor` 接口。

```cpp
class ThreadPoolScheduler : public async_simple::Executor, public Singleton<ThreadPoolScheduler> {
    // Init(thread_num)         — 初始化线程池（App::Init 中预创建）
    // Stop()                   — 停止并 join（关闭 Phase 4 调用）
    // Schedule(F&&)            — 提交任务（Executor 接口）
    // Dispatch(executor, f)    — 在指定 executor 上执行
    // Post(f)                  — 提交并立即触发
    // Execute(f)               — 同步语义执行
    // GetThreadNum()           — 线程数
};
```

**用法：** Worker 帧循环中发现耗时操作（寻路、批量计算、加解密等），把任务丢给 ThreadPoolScheduler，完成后的回调再投递回 Worker 队列，避免阻塞业务帧循环。

```cpp
// 在某个 Normal Worker 中
w->Post([]() {
    ThreadPoolScheduler::Instance()->Schedule([w]() {
        auto result = HeavyCompute();          // 线程池线程执行
        w->Post([result]() {                   // 完成回调回投 Worker
            OnComputeDone(result);
        });
    });
});
```

**关闭顺序：** Phase 3 会先 Drain ThreadPool，确保这些回投任务已进入 Worker 队列，再进入 Worker 任务清空流程。

## 跨线程投递

| 调用 | 线程 | 说明 |
|---|---|---|
| `worker->Post(f)` | 任意线程 | 投递到目标 Worker 队列，帧循环内执行 |
| `WorkerManager::PostToMain(f)` | 任意线程 | 投递到主线程 Worker |
| `WorkerManager::BroadcastToWorkers(f)` | 任意线程 | 广播到所有 Normal Worker |
| `IoWorker → Router → Worker::Post` | IoWorker | 网络消息分发（见模块五） |

---

# 模块三：传输层（TCP / SSL / KCP）

`Server` / `Client` 通过 `transport_type` 选择传输层，`ByteStream` 构造时按类型创建对应的 `ITransport` 实现（`src/message_stream/byte_stream.cpp`）：

```cpp
// src/define/define.h
enum class TRANSPORT_TYPE : int8_t
{
    TCP = 0,   // 明文 TCP
    SSL = 1,   // TLS over TCP
    KCP = 2,   // 可靠 UDP
};
```

| 传输层 | 底层协议 | 加密 | 典型场景 |
|---|---|---|---|
| `TCP` | TCP | 无 | 内网通信、默认传输 |
| `SSL` | TCP + TLSv1.2 | 是（服务端证书） | 公网传输、账号/充值等敏感数据 |
| `KCP` | UDP（可靠重传） | 无 | 弱网、实时对战、延迟敏感链路 |

## TCP（明文 TCP）

默认传输层（`TRANSPORT_TYPE::TCP`），走标准 `async_accept` / `async_connect`，无加密开销。适合内网或明文不敏感的数据。

## SSL（TLS over TCP）

- TLSv1.2（禁用 SSLv2/3），握手由框架自动完成：
  - 服务端：`Listener::on_accept` → `async_handshake(stream_base::server)` → `set_socket_connected`；
  - 客户端：`SslTransport::async_connect` → connect → handshake(client) → `set_socket_connected`。
- 证书配置：
  - 服务端 `ServerOptions.ssl_cert_file` + `ssl_key_file`（证书链 + 私钥，`use_certificate_chain_file` + `use_private_key_file`）；
  - 客户端 `ClientOptions.ssl_ca_file`（CA 证书，`load_verify_file` + `verify_peer` 校验服务端证书链）。
- ⚠️ 服务端证书必须由客户端信任的 CA 签发且**在有效期内**——证书过期/链不匹配时握手失败（`NET_LOG_ERROR` 记录 `handshake error`，见模块十）。自签名测试证书见 `res/ssl/`（ca/server/client × .crt/.key，有效期 10 年）。

## KCP（可靠 UDP）

基于 ikcp 的可靠 UDP 传输，在 UDP 之上实现重传、乱序重组与流量窗口（`src/message_stream/transport/kcp_transport.h`）：

- **无连接语义**：客户端连接时生成唯一 `conv`（`GenerateKcpConv`，碰撞概率 ~1/2³²），服务端按 conv 区分连接。
- **服务端模型**：`KcpListener` 只 bind 一个 UDP socket（`reuse_address`），收包后按 `ikcp_getconv` 路由到对应会话的 io_context（每连接独立 io_service，多连接复用同一端口）；首包到达时创建 Session。
- **关键参数**：mss 1376B、收发窗口 128/128（≈176KB，读回压上限）、update 间隔 10ms、`ikcp_nodelay` 快速模式、接收缓冲 64KB。
- **数据通路**：发送 `async_write_some` → `_out_buffer` → 按 mss 分段 `ikcp_send`（发送窗口满则 hold，10ms 定时器推进）；接收常驻 `async_receive_from` → `ikcp_input` → `ikcp_recv` → `_in_buffer` → `async_read_some` 交付。
- **不加密**：KCP 不承载 TLS，公网边界需外层加密或消息级 AEAD。
- **流控**：写限流直接生效（配额不足排队，充值后继续）；读限流间接生效——流控停取数据后，由 KCP 接收窗口回压对端。

## 配置示例

```cpp
// 服务端：SSL（TCP/SSL/KCP 三选一）
gb::ServerOptions options;
options.transport_type = gb::TRANSPORT_TYPE::SSL;
options.ssl_cert_file  = ResPath::Instance()->FindResPath("ssl/server.crt");
options.ssl_key_file   = ResPath::Instance()->FindResPath("ssl/server.key");
gb::Server server(options);

// 客户端：SSL
gb::ClientOptions options;
options.transport_type = gb::TRANSPORT_TYPE::SSL;
options.ssl_ca_file    = ResPath::Instance()->FindResPath("ssl/ca.crt");
gb::Client client(options);
```

---

# 模块四：消息系统

## 消息头（MessageHeader）

每个 TCP 消息以 **24 字节**消息头开始（`src/message_stream/message_header.h`）：

```cpp
#define MESSAGE_HEAD_MAGIC 322122u

struct MessageHeader {          // 24 字节（未打包，按 8 对齐）
    union {
        char     magic_str[4];
        uint32_t magic_str_value;   // 4B  魔数，固定 322122
    };
    int32_t  meta_size;             // 4B  Meta 序列化字节数
    int64_t  data_size;             // 8B  消息体字节数
    int64_t  message_size;          // 8B  校验：message_size == meta_size + data_size
};
```

接收侧 `CheckMagicString()` 校验魔数；`message_size == meta_size + data_size` 防止粘包/断包解析错误。

## 消息元信息（Meta）

消息体由 **Meta + protobuf 数据** 组成。Meta 定义在 `src/network/io/message_meta.h`，`#pragma pack(1)` 紧凑布局：

```cpp
enum class MsgMode : uint8_t {
    Msg      = 0,    // 普通消息（按 type 分发）
    Request  = 1,    // RPC 请求（按 method MD5 分发）
    Response = 2,    // RPC 响应（按 sequence 回填）
};

enum class CompressType : uint8_t {
    CompressTypeNone = 0,
    CompressTypeGzip = 1,
    CompressTypeZlib = 2,
    CompressTypeLZ4  = 3,
};

struct Meta {
    MsgMode      mode;           // 消息模式
    uint64_t     user_unique_id; // 实体 ID（路由键：0=系统消息走主线程）
    uint32_t     type;           // 普通消息类型（Listen 分发）
    CompressType compress_type;  // 压缩类型（gzip/zlib/LZ4）
    uint64_t     method;         // RPC 方法名 MD5 hash
    uint64_t     sequence;       // RPC 序列号（编码 worker_index + local_seq）
};
```

## 传输缓冲（src/buffer/）

序列化产物通过**块式零拷贝缓冲**在 IO 线程与 Worker 之间传递，避免大数据拷贝：

- **ReadBuffer** — 实现 protobuf `ZeroCopyInputStream`：`Next()/BackUp()/Skip()/ByteCount()`；块由 `BufferHandle` 组织，**引用计数**共享，多处持有同一块内存不拷贝。
- **WriteBuffer** — 实现 protobuf `ZeroCopyOutputStream`：`Reserve(n)` 预留、`WriteRaw` 写入、`SwapOut(ReadBuffer*)` 零拷贝转交给读侧。
- **tran_buf_pool** — 传输缓冲池，接收侧复用缓冲块（`_tran_buf` + 偏移指针），减少内存分配。
- **compressed_def.h** — `CompressType` 定义（同上）。

```cpp
// 序列化 protobuf 消息 → WriteBuffer → SwapOut 交给发送管线
WriteBuffer write_buffer;
msg.SerializeToZeroCopyStream(&write_buffer);
ReadBufferPtr read_buffer(new ReadBuffer());
write_buffer.SwapOut(read_buffer.get());
// 缓冲随后进入发送管线（RPC 的 CallImpl(meta, call, buffer) 即此模式；
// 普通消息发送直接传 protobuf Message，见下文"发送消息"）
```

## 压缩（gzip / zlib / LZ4）

`src/buffer/` 下提供三种压缩流实现：

| 文件 | 压缩 | 依赖 |
|---|---|---|
| `gzip_stream.h/.cpp` | gzip | zlib |
| `lz4_stream.h/.cpp` | LZ4（最快） | LZ4 |
| `compressed_stream.h` | 按 `CompressType` 创建对应压缩流 | — |

**流程：** 发送时按 `meta.compress_type` 压缩消息体；接收时 `identify_message_header` 读出头后，按压缩类型解压还原。压缩类型通过 `Meta.compress_type` 逐消息指定。

## 流量控制（src/message_stream/flow_controller.h）

```cpp
class FlowController {
    void reset_read_quota(bool read_no_limit, int quota);     // 重置读取配额
    void reset_write_quota(bool write_no_limit, int quota);   // 重置写入配额
    void recharge_read_quota(int quota);                      // 充值读取配额
    void recharge_write_quota(int quota);                     // 充值写入配额
    bool has_read_quota() const;  bool has_write_quota() const;
    int  acquire_read_quota(int quota);   // >0 成功；<=0 失败（返回值可作为触发顺序序列号）
    int  acquire_write_quota(int quota);
};
```

- 读/写配额独立控制，`no_limit` 关闭限制；
- 配额不足时 `acquire_*` 返回负序列号，调用方按"越接近零越早触发"排队等待充值；
- 防止单个连接突发数据压垮接收缓冲 / 发送缓冲无界增长。

## MessageStream（连接级消息流，src/message_stream/）

一个连接上的**消息读写编排器**，继承自 `ByteStream`（`USER_SSL_SOCKET` 宏开启时继承 `SSLByteStream`）：

| 成员 | 说明 |
|---|---|
| `async_send_message(ReadBufferPtr)` | 投递一条待发送消息 |
| `set_flow_controller(ptr)` | 绑定流控（配额不足自动挂起） |
| `set_max_pending_buffer_size(n)` | 发送缓冲上限（超过则拒绝/挂起） |
| `pending_message_count / pending_data_size / pending_buffer_size` | 待发送统计 |
| `set_ssl_server_file_path / set_ssl_client_file_path` | SSL 证书路径（HTTPS/SSL 连接） |
| `on_sending / on_sent / on_send_failed / on_received` | 虚回调，由 Session 实现 |

**收包流程：** `on_read_some` → 缓冲 → `identify_message_header` 识别头 → `split_and_process_message` 拆包校验 → `on_received(message, meta_size, data_size)`。

**发包流程：** `async_send_message` → 流控 + 待发送队列（`_pending_calls`/`_swapped_calls` 双队列）→ `trigger_send` → `on_sending` → `on_write_some` 部分写完 → 全量写完 `on_sent`。

**Token 并发保护：** `_send_token`/`_receive_token`（TOKEN_FREE/TOKEN_LOCK）确保同一连接在同一时刻只有一个发送/接收流程在推进。

## 发送消息

```cpp
// C++ — 完整 Meta
gb::Meta meta;
meta.mode           = gb::MsgMode::Msg;
meta.user_unique_id = entity_id;
meta.type           = MT_EnterScene;
gb::NetworkManager::Instance()->Send(session, meta, msg);

// C++ — 快捷重载（type + id）
gb::NetworkManager::Instance()->Send(session, MT_EnterScene, entity_id, msg);
```

```lua
-- Lua — BuildMeta 构造 Meta 后发送（MsgMode/CompressType 枚举已注册）
local meta = net.BuildMeta({
    mode = MsgMode.Msg,
    user_unique_id = 10001,
    type = 10001,
    compress_type = CompressType.None,
})
net.Send(session, meta, msg)

-- Lua — 快捷重载
net.Send(session, 10001, 10001, msg)      -- (session, type, id, proto_msg)
net.Send(session, meta, msg)              -- (session, meta, proto_msg)

-- 反向：ParseMeta 把二进制 Meta 解析回 Lua table
local tbl = net.ParseMeta(meta_bytes)
```

`net.Send` 同时注册了 `Session*` 与 `std::shared_ptr<Session>` 两种重载，Lua 侧 `session` 参数均可使用。

## 注册与接收消息

```cpp
// C++ — 处理器直接接收解包后的 protobuf 消息（框架自动按 protoName 反序列化）
void OnEnterScene(TestMsg& msg) {
    LOG_INFO("enter scene, index:{}", msg.index());
}

// C++ — 注册消息处理（必须在目标 Worker 线程内注册）
w->Post([]() {
    gb::NetworkManager::Instance()->Listen(MT_EnterScene, OnEnterScene, "TestMsg");
});

// C++ — 反注册
gb::NetworkManager::Instance()->UnListen(MT_EnterScene, "signal", 0);

// Lua
net.Listen(10001, function(session, msg)
    log.Info("receive msg")
end, "TestMsg")

net.UnListen(10001, "signal", 0)
```

**接收完整流程：**

```
IoWorker 收到完整消息
  └─ Session::_received_callback
       └─ NetworkManager::OnReceiveCall(meta, data)
            └─ NetworkManager::Dispatch
                 ├─ MsgMode::Msg      → FindListenFunction(meta.type)
                 │                      → Router::GetExecutor(type, uid) → Worker::Post
                 ├─ MsgMode::Request   → FindRpcFunction(meta.method)
                 │                      → Router::GetExecutor(type, uid) → Worker::Post
                 └─ MsgMode::Response  → 从 sequence 解码 worker_index
                                         → Worker::Post → TakePendingRpc(local_seq)
                                         → RpcCall::Done → 回调
```

---

# 模块五：路由机制

## 路由总览

| 特性 | Msg | RPC Request/Response | HTTP |
|---|---|---|---|
| 路由键 | `meta.type` (uint32) | Request: `meta.method` (MD5) / Response: `meta.sequence` | URL path + method |
| 路由表 | `ListenMap` (type → func) | `RpcInterfaceMap` (method_hash → func) | `vector<RouteEntry>` |
| Worker 选择 | `Router::GetExecutor` | 同左 | IO 线程直接执行 |
| 序列化 | protobuf | msgpack / protobuf | JSON/文本 |

## Router（src/network/router/）

Router 是消息 → Worker 的**统一入口**，带路由策略与双缓冲冻结机制：

```cpp
class Router {
    void SetServiceTypeResolver(std::function<ServiceWorkerType(MessageType)>);   // MessageType → ServiceWorkerType
    void SetRouteKeySelector(std::function<uint64_t(MessageType, uint64_t)>);     // (type, uid) → 路由键（默认取 uid）
    void SetWorkerIndexSelector(std::function<size_t(const std::vector<WorkerWeakPtr>&, MessageType, uint64_t)>); // 自定义 Worker 选择
    void RegisterWorker(ServiceWorkerType, Worker*);
    void BindSingleEntity(uint64_t entity_id, uint32_t worker_index);             // 绑定单个实体 → Worker
    void BindEntity(uint64_t entity_begin, uint64_t entity_end, uint32_t worker_index); // 绑定实体区间 → Worker
    void UnbindEntity(uint64_t entity_id);
    void Freeze();               // 冻结 RouteTable + 路由策略（启动完成后调用）
    void FreezeEntityRoutes();   // 发布实体路由快照（主循环每帧调用，双缓冲 swap）
    WorkerExecutor GetExecutor(uint32_t message_type, uint64_t user_unique_id) const;
};
```

### 策略路由（Policy）

```cpp
enum class Policy { Stateful, Stateless };
```

- **Stateful（有状态）：** `user_unique_id > 0` 时走 **实体路由表**（`BindEntity` 绑定实体 → Worker），路由未命中**丢弃**消息（实体可能已迁移/销毁，不该散落到别的线程）。`user_unique_id == 0` 的**系统消息固定走主线程 Worker**。
- **Stateless（无状态）：** 纯 hash 取模，任何 Worker 都能处理。

```
GetExecutor(message_type, user_unique_id)
  ├─ user_unique_id == 0 → Main Worker（系统消息）
  ├─ Policy::Stateful
  │    ├─ 实体已绑定 → 实体对应的 Worker
  │    └─ 未绑定     → 丢弃消息（保证实体消息不串线程）
  └─ Policy::Stateless
       └─ hash(user_unique_id) % workers.size()
```

### 实体路由表（LockFreeRouteTable）

- 双缓冲（读/写两份）：**写侧仅主线程**（Bind/Unbind/Freeze），读侧是任何线程；
- `FreezeEntityRoutes()` 在主循环每帧调用，原子 swap 发布快照，**读侧无锁**；
- 表内实体排序后**二分查找**，适合大量实体绑定（如场景内实体 → 场景 Worker）。

### ServiceWorkerType 分类

```cpp
enum class ServiceWorkerType {
    SWT_Normal     = 0,
    SWT_AI         = 1,
    SWT_Navigation = 2,
};
// 默认解析规则：MessageType % 10000 → ServiceWorkerType
//   0/10000/20000 % 10000 = 0 → Normal
//   10001      % 10000 = 1 → AI
//   20001      % 10000 = 2 → Navigation
```

### 自定义路由策略

三个可插拔回调：

1. **SetServiceTypeResolver** — 自定义 `MessageType → ServiceWorkerType`（取代 `% 10000` 默认规则）
2. **SetRouteKeySelector** — 自定义 `(message_type, user_unique_id) → 路由键`（默认取 `user_unique_id`）
3. **SetWorkerIndexSelector** — 自定义 Worker 选择：`(workers, message_type, user_unique_id) → worker_index`（取代默认 `hash % size`）

### Worker 选择（PickWorker）

默认：`route_key % workers.size()`；有 `SetWorkerIndexSelector` 时按其返回下标。

## RPC 响应路由

响应消息不经过 Router 策略——`sequence` 高 32 位编码 `worker_index`，NetworkManager 直接解码并投递到对应 Worker，由 `TakePendingRpc(local_seq)` 取出挂起的 RpcCall 触发回调。

---

# 模块六：RPC 系统

RPC 基于 **Meta(MsgMode::Request/Response) + MD5 方法路由 + msgpack 序列化**，支持回调与协程两种调用方式。

## 方法路由

- 方法名 → `MD5::MD5Hash64(method)`（`src/base/md5.hpp`）作为 `Meta.method`，注册与调用两侧计算一致即命中；
- 注册表 `RpcInterfaceMap`（method_hash → 处理函数）；
- 参数序列化：多参数用 msgpack pack；单 protobuf 消息参数直接序列化。

## 注册

```cpp
// C++ — 在目标 Worker 线程内注册
w->Post([]() {
    gb::NetworkManager::Instance()->Register("method_name",
        [](gb::RpcReply reply, int arg1, std::string arg2) {
            // 处理参数，返回结果
            reply.Invoke(result);
        });
});
```

```lua
-- Lua
net.Register("method_name", function(reply, arg1, arg2)
    reply:Invoke(result)
end)
```

## 调用

```cpp
// C++ — 回调风格
auto call = std::make_shared<gb::RpcCall>();
call->SetTimeout(3000);
call->SetCallBack([](int result) { /* 业务线程回调 */ });
gb::NetworkManager::Instance()->Call(call, "method_name", 0, arg1, arg2);

// C++ — 协程风格（async_simple）
auto result = co_await gb::CoRpc<int>::execute(
    std::make_shared<gb::RpcCall>(), "method_name", 0, arg1, arg2);
```

```lua
-- Lua — 协程风格（rpc.Await 桥接，必须在 coroutine 内调用）
-- rpc.Await(method, id, setup, ...) → err, result
local err, result = rpc.Await("method_name", 0, function(call)
    call:SetTimeout(3000)
end, arg1, arg2)
```

```lua
-- Lua — 回调风格
local call = RpcCall.new()
call:SetSession(session)
call:SetTimeout(3000)
call:SetCallBack(function(reply, err, ...)
    -- 处理响应
end)
net.Call(call, "method_name", 0, arg1, arg2)
-- 或使用完整 Meta
net.Call(call, meta, arg1, arg2)
```

## 核心类

| 类 | 说明 |
|---|---|
| `RpcCall` | RPC 请求：`SetSession/SetCallBack/SetTimeout/Cancel/SetId/GetId` |
| `RpcReply` | RPC 响应：`Invoke(...)` 打包 msgpack 回发 |
| `CoRpc<T...>` | 协程包装（async_simple `Lazy<T>`） |
| `WorkerExecutor` | 把任务派发到 Worker 线程（RPC 注册/调用上下文） |
| `GbAsyncExecutor` | async_simple 执行器适配器 |
| `RpcTimerPool` | 每 IO 线程一个，负责 RPC 超时管理 |
| `ThreadLocalRpcContext` | 线程本地 RPC 上下文（注册表 + 待处理映射） |

**待处理映射**：每个 Worker 持有 `pending_rpcs_`（worker_index 编码在 sequence 高 32 位），响应到达时按 `local_seq` 直接取回 RpcCall 并触发回调——已经从 thread_local 迁移为 Worker 成员变量，为多线程池扩展做准备。

## Sequence 编码

```cpp
union SequenceId {
    struct { uint64_t index : 32; uint64_t seq : 32; };
    uint64_t value;
};
// 高 32 位 = worker_index，低 32 位 = local_seq
```

## 处理器辅助（function.hpp / rpc_function.hpp）

- `net_listen_fun` / `rpc_listen_fun` — 消息/RPC 处理函数封装；
- `MakeNetHandler` / `MakeRpcHandler` — 从任意可调用对象生成处理器；
- `NetFunctionaTraits` / `RpcFunctionaTraits` — 参数/返回值类型推导（支持 Lua sol::function 与 C++ lambda）；
- `rpc_function_help.h` — `function_traits`、`tuple_tail_t`、`GetMsgData` 等元编程辅助。

---

# 模块七：Lua 脚本系统

每个 Worker 拥有**独立的 Lua 状态**（sol3），绑定互不干扰。

## Lua 全局表

| 表 | 来源 | 内容 |
|---|---|---|
| `log` | register_log | `Info/Error/Warning`（自动带 Lua 文件:行号） |
| `msgpack` | register_msgpack | `pack(...)` / `unpack(data)` |
| `net` | register_net + RegisterRpcLua | `Listen/UnListen/Send/Register/Call/BuildMeta/ParseMeta` |
| `rpc` | RegisterRpcLua | `Await(method, id, setup, ...)` 协程桥 |
| `timer` | register_timer | `Register/RegisterSystem/UnRegister` |
| `redis` | register_redis | 连接池 + 异步 + `Await` |
| `pg` | register_postgresql | 异步 + `Await` |
| `nats` | register_nats | `Connect/Publish/Reply/Subscribe/AsyncRequest` |
| `etcd` | register_etcd | `Connect/Put/Get/Delete/Watch/Await` |
| `Session` | register_net | 会话对象（不透明，用于 Send） |
| `RpcCall` / `RpcReply` | RegisterRpcLua | RPC 调用/响应对象 |
| `MsgMode` / `CompressType` | register_net | 枚举 |
| `Meta` | register_net | 消息元信息（字段可读写） |
| `create_msg(proto_name)` | register_script | 创建 protobuf 消息（未注册返回 nil + 日志） |
| `vec_uint8` | register_msgpack | 字节数组类型 |

## net 表详细

```lua
-- 消息收发
net.Listen(type, func, "ProtoName")     -- 注册消息处理（protoName 可选）
net.UnListen(type, signal, level)       -- 反注册
net.Send(session, type, id, msg)        -- 发送（快捷）
net.Send(session, meta, msg)            -- 发送（完整 Meta）
net.BuildMeta({...}) → bytes            -- Lua table → 二进制 Meta
net.ParseMeta(bytes) → table            -- 二进制 Meta → Lua table

-- RPC
net.Register("method", func)            -- 注册 RPC 方法
net.Call(call, "method", id, ...)       -- 发起 RPC 调用
net.Call(call, meta, ...)               -- 发起 RPC 调用（完整 Meta）
```

```lua
-- Meta usertype（字段可读写）
local m = Meta.new()
m.mode          = MsgMode.Request
m.user_unique_id = 10001
m.type          = 0
m.method        = 0          -- RPC 方法 hash（net.Call 时自动填）
m.sequence      = 0          -- 序列号（框架自动填）
m.compress_type = CompressType.None
print(tostring(m))           -- Meta{mode=1, ...}
```

## 脚本加载

```
Worker::OnStart()
  └─ InitLua()
       ├─ open_libraries()                    — base/package/string/table/os/bit32/coroutine/debug/ffi/io/jit/math/utf8
       ├─ _lua_(scriptPtr)                    — 注册上表全部 C++ 绑定
       ├─ package.cpath += exe 目录（?.so）    — LuaSocket 所在路径
       ├─ require("socket.core")
       ├─ package.path += ../script/?.lua
       ├─ safe_script_file("start_debug.lua") — LuaPanda 调试器（127.0.0.1:8828，可选）
       └─ require("main.lua")                 — 用户脚本入口
```

## 调试

- `script/start_debug.lua` — 启动 LuaPanda 调试器（默认 127.0.0.1:8828），可用 VS Code 附加调试；脚本缺失或加载失败仅告警，不影响启动。
- `script/main.lua` — 示例：定时器注册、msgpack 自测、RPC 注册、Listen 注册。
- `script/test.lua` — RPC 调用示例。

## protobuf ↔ Lua

- `src/script/lua_pb_parse.h/.cpp` — protobuf 消息与 Lua table 互转；
- `register_proto_msg.cpp` — 由 `res/proto/proto_to_sol.py` 生成的 sol2 绑定（`create_msg` 取 `_G[proto_name]`）；
- 协议代码生成：`res/proto/proto.sh`（protoc 生成 descriptors.pb + `protobuf/*.pb.h/.pb.cc`）→ `proto_to_sol.py` 生成 Lua 绑定 → `register_proto_msg.cpp`。

---

# 模块八：HTTP 模块

基于 **Boost.Beast** 的 HTTP/HTTPS 服务器与客户端，独立于 TCP 消息系统。

## HttpServer（src/network/http/http_server.h）

```cpp
// 启动 HTTP 服务（ip:port，默认 2 个 IO 线程）
bool HttpServer::Start(const std::string& ip, uint16_t port, int thread_count = 2);

// 启动 HTTPS 服务（加载 PEM 证书/私钥）
bool HttpServer::StartSSL(const std::string& ip, uint16_t port,
                          const std::string& cert_file,
                          const std::string& key_file,
                          int thread_count = 2);

// 路由注册：精确字符串路径匹配
void HttpServer::Get(const std::string& path, HttpRequestHandler handler);
void HttpServer::Post(const std::string& path, HttpRequestHandler handler);
void HttpServer::AddRoute(const std::string& path, boost::beast::http::verb method, HttpRequestHandler handler);

// 优雅停止（等待活跃会话结束）
void HttpServer::Stop();
```

**路由匹配：** 精确路径匹配，`GET/POST` 方法区分；匹配失败返回 404。路由表在请求处理时做只读快照，避免注册与查询并发问题。

**会话：** `HttpSession`（明文）/ `HttpsSession`（SSL），两者共用同一套 Dispatch 逻辑（`http_server_internal.h`）。

**LoginServer 用法（server/login_server/LoginApp.cpp）：**

```cpp
int LoginApp::OnServerInit()
{
    // HTTP 地址端口来自 server_config.xml（APP_LOGIN × win_http/linux_http）
    auto [http_ip, http_port_str] = AppTypeMgr::Instance()->GetServerIpPort(
        APP_LOGIN, UIR_TYPE::UT_LINUX_HTTP);

    // 注册登录路由
    http_server_.Post("/api/login", [](const gb::HttpRequest& req, gb::HttpResponse& res) {
        LOG_INFO("Login request: {}", req.body);
        res.SetJsonBody(R"({"code":0,"msg":"ok","data":{}})");
    });

    if (!http_server_.Start(http_ip, http_port, 2))
        return -1;
    return 0;
}
```

## HttpClient（src/network/http/http_client.h）

Boost.Asio 协程 + 回调两种 API：

```cpp
// 协程风格（Boost.Asio awaitable）
auto res = co_await client.Get("https://example.com/api");

// 回调风格
client->Get("http://example.com/api", [](gb::HttpResponse res) { ... });
client->Post("http://example.com/api", body, cb, "application/json");
```

## HTTP 消息类型（http_common.h）

`HttpRequest`（method/target/body/header）、`HttpResponse`（result/body/header + `SetJsonBody`）、`HttpRequestHandler = std::function<void(const HttpRequest&, HttpResponse&)>`。

---

# 模块九：定时器系统

## 双优先队列

每个 Worker 拥有独立 `TimerManager`，内部两棵**最小堆优先队列**：

| 队列 | 时钟 | 用途 |
|---|---|---|
| SteadyTimer | `steady_clock`（单调） | 游戏逻辑定时器（不受系统时间跳变影响） |
| SystemTimer | `system_clock`（真实时间） | 与真实世界时间对齐（跨天任务、定时维护等） |

`TimerManager::Update()` 由 Worker 帧循环每帧调用，将到期定时器回调批量派发；`EnterShutdownMode()` 取消未来定时器、完成当前帧（关闭 Phase 2）。

## C++ 接口

```cpp
// 注册（返回 timer_id）
int64_t TimerManager::RegisterTimer(int64_t milliseconds, std::function<void()> callback, bool loop = false);
int64_t TimerManager::RegisterSystemTimer(int64_t milliseconds, std::function<void()> callback, bool loop = false);
void    TimerManager::UnRegisterTimer(int64_t timer_id);
```

## Lua 接口（register_timer）

```lua
-- 注册到当前 Worker 的 TimerManager；回调参数为 timer_id
local id = timer.Register(1000, function(timer_id)      -- 稳态定时器，1 秒后触发
    log.Info("tick " .. timer_id)
    return true                                          -- 返回 true 继续循环（loop 参数也可控制）
end, true)

local sys_id = timer.RegisterSystem(60000, function(id)  -- 系统定时器，对齐真实时间
    log.Info("system tick")
end, true)

timer.UnRegister(id)                                     -- 反注册
```

**注意：** `timer.Register` 必须在 Worker 线程内调用（Lua 回调运行在所属 Worker 的帧循环中）。

---

# 模块十：错误处理与日志

## 日志（src/log/log.h）

spdlog 封装 `GbLog`：支持异步模式、控制台/文件输出、按天轮转（`res/log4/test_YYYY-MM-DD.log`）。

| 宏 | 级别 |
|---|---|
| `LOG_DEBUG` / `LOG_INFO` | 调试 / 信息 |
| `LOG_WARN` | 警告 |
| `LOG_ERROR` / `LOG_CRITI` | 错误 / 致命 |

```cpp
LOG_INFO("player {} enter scene {}", player_id, scene_id);   // fmt 风格变参
```

Lua 侧 `log.Info/Error/Warning` 自动携带 `文件:行号`（通过 `debug.getinfo` 定位）。

## CHECK 断言

`CHECK(expr)` / `CHECK_EQ/NE/LT/LE/GT/GE` — 失败记录错误日志，**不终止进程**（发布环境可用）。

## 错误码

- C++ 业务接口：返回 `0` 成功、`-1` 失败；
- 网络错误：`NET_ErrorCode`（`src/define/define.h`），覆盖连接失败、发送缓冲满、服务器关闭中、RPC 超时等；
- 数据库/NATS/ETCD：各自的 `*ErrorCode` 枚举，Lua 回调首参返回错误码，`0` 表示成功。

---

# 模块十一：Redis Lua 接口（src/db/redis/）

**连接池 + Boost.Redis**，Lua 侧支持**回调**与 **Await 协程**两种用法。

```lua
-- 连接（连接池，host, port, pool_size, password 可选）
redis.Connect("127.0.0.1", 6379, 4)
redis.IsHealthy()                      -- 健康检查

-- 回调风格
redis.AsyncGet("key", function(err, value)
    log.Info("get: " .. tostring(value))
end)

-- 协程风格（必须在 coroutine 内）
local err, value = redis.Await("Get", "key")
```

| 类别 | 接口 |
|---|---|
| 通用 | `Async(command, ...)`、`AsyncCall(...)`、`Connect`、`IsHealthy`、`Await(method, ...)` |
| String | `AsyncGet` `AsyncSet` `AsyncSetEx` `AsyncIncr` `AsyncIncrBy` `AsyncTTL` `AsyncExpire` `AsyncDel` `AsyncExists` |
| Hash | `AsyncHSet` `AsyncHGet` `AsyncHDel` `AsyncHKeys` `AsyncHVals` `AsyncHLen` |
| List | `AsyncLPush` `AsyncRPush` `AsyncLPop` `AsyncRPop` `AsyncLLen` |
| ZSet | `AsyncZAdd` `AsyncZRem` `AsyncZScore` `AsyncZRank` `AsyncZRevRank` `AsyncZCount` `AsyncZCard` `AsyncZIncrBy` `AsyncZRange` `AsyncZRevRange` `AsyncZRangeByScore` `AsyncZRevRangeByScore` `AsyncZRangeWithScores` `AsyncZRevRangeWithScores` `AsyncZRemRangeByRank` `AsyncZRemRangeByScore` |
| 脚本 | `AsyncEval` |

集成测试：`test/db_test/`（独立二进制）+ `script/test_db.lua`。

---

# 模块十二：PostgreSQL Lua 接口（src/db/postgres/）

**libpq**，异步连接与查询，支持事务与 Await 协程。

```lua
pg.AsyncConnect("127.0.0.1", 5432, "dbname", "user", "password", function(err)
    log.Info("pg connected: " .. tostring(pg.IsConnected()))
end)

-- 协程风格
local err, rows = pg.Await("Query", "SELECT * FROM player WHERE id = $1", 1001)
```

| 接口 | 说明 |
|---|---|
| `AsyncConnect(host, port, dbname, user, password, callback)` | 异步连接 |
| `IsConnected()` | 连接状态 |
| `AsyncQuery(sql, ...params)` / `AsyncExecute(sql, ...)` | 查询 / 执行 |
| `AsyncBegin` / `AsyncCommit` / `AsyncRollback` | 事务控制 |
| `AsyncClose` | 关闭连接 |
| `Async(method, ...)` / `Await(method, ...)` | 通用异步 / 协程桥 |

集成测试：`test/db_test/` + `script/test_db.lua`。

---

# 模块十三：NATS 消息系统（src/network/nats/）

基于 **cnats（NATS C 客户端）** 的异步消息系统，支持**普通消息 + 请求/响应**，与 `Meta` 无缝对接。

## 配置（res/config/nats_config.xml）

```xml
<root>
    <nats>
        <url>nats://127.0.0.1:4222</url>
        <num_pending_publishes>1024</num_pending_publishes>
    </nats>
</root>
```

## Lua 接口

```lua
-- 连接 / 状态
nats.Connect("nats://127.0.0.1:4222")
nats.IsConnected()
nats.Disconnect()

-- 发布（三种重载：原始数据 / Meta+数据 / protobuf 消息）
nats.Publish("subject", data_str)
nats.Publish("subject", meta_bytes, data_str)
nats.Publish("subject", meta_bytes, proto_msg)     -- 自动序列化

-- 响应（在订阅回调中回复，同三种重载）
nats.Reply("subject", data_str)
nats.Reply("subject", meta_bytes, data_str)
nats.Reply("subject", meta_bytes, proto_msg)

-- 订阅（回调风格）
nats.Subscribe("subject", function(msg_str) ... end)
nats.Subscribe("subject", meta_bytes, function(meta, msg_str) ... end)
nats.Subscribe("subject", "ProtoName", function(msg) ... end)

-- 请求/响应（NATS request-reply）
nats.AsyncRequest(subject, meta_bytes, data_str, function(err, body_str) end, timeout_ms)
nats.AsyncRequestMsgpack(subject, meta_bytes, ..., function(err, ...) end, timeout_ms)
nats.AsyncRequestProto(subject, meta_bytes, request_proto, "ResponseProto", function(err, resp) end, timeout_ms)

-- 协程风格（必须在 coroutine 内）
local err, body = nats.Await("Request", subject, meta_bytes, data_str, 3000)
local err, ...  = nats.Await("RequestMsgpack", subject, meta_bytes, ...)
local err, resp = nats.Await("RequestProto", subject, meta_bytes, request_proto, "ResponseProto")
```

**注意：** NATS 订阅回调运行在**主线程 Worker** 帧循环中（`user_unique_id == 0` 系统消息），回调内可直接操作主线程数据，跨线程操作需 `Post` 到目标 Worker。

---

# 模块十四：ETCD 服务发现（src/network/etcd/）

基于 **etcd v3 HTTP JSON API**（使用框架自研 HttpClient，底层 Boost.Asio）的 KV + 租约 + Watch，用于服务注册与发现。

## Lua 接口

```lua
-- 连接（endpoints 逗号分隔；user/password 可选）
etcd.Connect("http://127.0.0.1:2379")
etcd.IsConnected()
etcd.Disconnect()

-- KV（同步语义内部异步，结果回投）
etcd.Put(key, value)
etcd.PutWithLease(key, value, lease_id)
etcd.PutWithTTL(key, value, ttl_seconds)
etcd.Get(key)
etcd.Delete(key)

-- 租约
etcd.GrantLease(ttl_seconds)               -- 返回 lease_id

-- 监听（key 前缀匹配，回调在系统消息中执行）
etcd.Watch("prefix/", function(change_type, key, value) ... end)
etcd.Unwatch(key_prefix)
etcd.Update(key_prefix)                    -- 手动拉取刷新

-- 异步 + 协程
etcd.AsyncPut/AsyncPutWithLease/AsyncPutWithTTL/AsyncGet/AsyncDelete/AsyncGrantLease(..., callback)
etcd.Async(method, ...)
etcd.Await("Put", key, value)              -- 协程风格
```

**典型用法：** 服务启动 `PutWithTTL` 注册自身 → `Watch` 服务前缀发现对端 → 租约过期自动摘除（心跳由框架续约）。

**注意：** ETCD 回调同样运行在**主线程 Worker**（系统消息通道）。

---

# 目录结构

```
src/
  app/                   — App 基类（帧循环、生命周期、4 阶段优雅关闭）
    app.h/.cpp               — App: Init/Run/Stop
    server_app.h/.cpp        — ServerApp: CLI 解析(-t/-r)、Worker/Router 引导、TCP Server 启动
    types.h/.cpp             — APP_TYPE 枚举、AppTypeMgr、UIR_TYPE（pugixml 解析 server_config.xml）
  async/                 — 异步基础设施
    shutdown.h/.cpp          — ShutdownManager（4 阶段关闭状态机）
    signal_handler.h/.cpp    — SignalHandler（Ctrl+C / SIGTERM）
    thread_pool_scheduler.h  — ThreadPoolScheduler（重度计算线程池，async_simple::Executor）
  base/                  — 共享工具
    singleton.h              — Singleton<T>
    res_path.h/.cpp          — 资源路径解析
    md5.hpp                  — constexpr MD5（Hash32/Hash64，RPC 方法路由）
    timer_help.h             — CHRONO_SECOND/MICROSECOND 宏
    util_string.h            — 字符串工具
    endpoint_help.h          — 网络端点解析
  buffer/                 — 传输缓冲 + 压缩（NEW）
    buffer.h/.cpp            — ReadBuffer/WriteBuffer（protobuf ZeroCopy 流，块式零拷贝）
    buffer_handle.h          — BufferHandle（缓冲块，引用计数共享）
    block_wrappers.h/.cpp    — 缓冲块包装（ZeroCopy 接口实现）
    compressed_def.h         — CompressType（None/Gzip/Zlib/LZ4）
    compressed_stream.h/.cpp — 压缩流工厂
    gzip_stream.h/.cpp       — gzip 压缩流
    lz4.h/.cpp               — LZ4 压缩流
    tran_buf_pool.h          — 传输缓冲池（接收侧内存复用）
  db/
    redis/                   — Redis 连接池 + Lua 绑定（Boost.Redis）
    postgres/                — PostgreSQL + Lua 绑定（libpq）
  define/
    define.h                 — NET_TYPE（含 NT_NATS）、NET_ErrorCode、通用宏（NEW）
  log/
    log.h/.cpp               — GbLog（spdlog）、LOG_* 宏、CHECK 宏
    net_log_help.h           — 网络日志宏
  message_stream/         — 连接级消息流（NEW）
    message_stream.h/.cpp    — MessageStream（读写编排、待发送队列、拆包）
    message_header.h         — MessageHeader（magic/meta_size/data_size/message_size）
    flow_controller.h/.cpp   — FlowController（读写配额流控）
    byte_stream.h            — ByteStream（TCP 流抽象）
    ssl_byte_stream.h/.cpp   — SSLByteStream（SSL/TLS 流）
  network/
    http/                    — HTTP 服务器/客户端（Boost.Beast）
    io/                      — 网络 IO 层
      server.h/.cpp              — Server/ServerImpl（TCP 服务器）
      client.h/.cpp              — Client/ClientImpl（TCP 客户端）
      listener.h/.cpp            — Listener（accept 循环）
      session.h/.cpp             — Session（MessageStream 包装、心跳、回调）
      io_service_pool.h/.cpp     — IoServicePool/IoWorker（io_context 池）
      timer_worker.h/.cpp        — TimerWorker（asio 周期定时器）
      message_meta.h             — Meta、MsgMode、CompressType、ReadMeta
      handle_interface.h         — HandleInterface（回调设置接口）
    manager/
      network_manager.h/.cpp    — NetworkManager（单例：Send/Listen/Register/Dispatch/Freeze）
    msgpack/                   — msgpack 序列化（pack/unpack + sol 绑定）
    nats/                      — NATS 客户端（NEW）
      nats_manager.h/.cpp          — NatsManager（连接、发布、订阅、请求响应）
      register_nats.cpp            — nats.* Lua 绑定（含 nats.Await 协程桥）
    etcd/                      — ETCD 服务发现（NEW）
      etcd_manager.h/.cpp          — EtcdManager（KV/租约/Watch）
      register_etcd.cpp            — etcd.* Lua 绑定
    router/                    — 消息路由
      message_type.h              — MessageType 枚举
      route_table.h/.cpp          — RouteTable（路由表 + FrozenSnapshot 冻结快照）
      lock_free_route_table.h     — 双缓冲无锁实体路由表（header-only）
      router.h/.cpp               — Router（GetExecutor、策略、冻结）
      service_worker_type.h       — ServiceWorkerType（Normal/AI/Navigation）
    rpc/                       — RPC 系统
      executor.h                  — WorkerExecutor、GbAsyncExecutor
      function.hpp                — net_listen_fun、MakeNetHandler
      rpc_function.hpp            — rpc_listen_fun、MakeRpcHandler
      rpc_function_help.h/.cpp     — function_traits 等元编程
      rpc_call.h/.cpp             — RpcCall、SequenceId、RpcErrorCode
      rpc_reply.h/.cpp            — RpcReply（msgpack 打包响应）
      rpc_coro.h                  — CoRpc<T...>（async_simple 协程）
      rpc_threadlocal.h/.cpp      — ThreadLocalRpcContext
      rpc_timer_pool.h/.cpp       — RpcTimerPool（每 IO 线程超时管理）
      register_rpc.h/.cpp         — net.Register/Call、RpcCall/RpcReply、rpc.Await
  script/                  — Lua 脚本系统
    script.h/.cpp               — Script（sol::state 扩展）、ProtoEncode/ProtoDecode
    register_script.h/.cpp      — _lua_ 入口（log/msgpack/proto/net/rpc/redis/pg/nats/etcd/timer）
    register_proto_msg.cpp      — protobuf 消息 sol2 绑定（proto_to_sol.py 生成）
    lua_pb_parse.h/.cpp         — protobuf ↔ Lua table 转换
  timer/                   — 定时器
    timer.h/.cpp                — Timer 基类、SteadyTimer、SystemTimer
    timer_manager.h/.cpp        — TimerManager（双优先队列、注册/反注册、关闭支持）
    register_timer.cpp          — timer.* Lua 绑定
  worker/                  — Worker 系统
    worker.h/.cpp               — Worker（帧循环、Post、Lua、RPC 序列号、关闭）
    worker_manager.h/.cpp       — WorkerManager（InitMainWorker/CreateWorker/PostToMain/Broadcast）
    worker_logic_interface.h    — IWorkerLogic（OnStartup/OnUpdate/OnTick/OnCleanup）

test/
  server_test/             — server_test 二进制（App 直接派生）
  client_test/             — client_test 二进制（RPC 协程示例）
  db_test/                 — db_test 二进制（Redis/PG/Lua 集成测试，无 App）
  unit_test/               — unit_test 二进制（Catch2：msgpack/路由/调度器/定时器/App）

server/
  login_server/            — login_server（ServerApp + HttpServer）
  gateway_server/          — gateway_server
  scene_server/            — scene_server

script/                    — Lua 脚本
  main.lua                 — Worker 启动加载（定时器/msgpack/RPC/Listen 示例）
  start_debug.lua          — LuaPanda 调试器（127.0.0.1:8828）
  test.lua                 — RPC 调用测试
  test_db.lua              — Redis + PG 集成测试
  db_test_lua_redis_pg.lua — db_test 加载的完整 Redis/PG 绑定测试
  db_test_lua_test.lua     — db_test 加载的 C++ 绑定测试
  LuaPanda.lua             — LuaPanda 调试库

res/
  config/
    server_config.xml      — 服务器 IP/端口（win/linux × tcp/http × APP_TYPE）
    nats_config.xml        — NATS 地址与待发布队列大小
  proto/
    meta.proto             — Meta 协议定义
    msg.proto              — TestMsg 示例协议
    proto.sh               — protoc 代码生成（descriptors.pb + protobuf/*.pb.h/.pb.cc）
    proto_to_sol.py        — sol2 绑定生成器
  ssl/                     — 自签名 CA/服务器/客户端证书
  log4/                    — 运行时日志

protobuf/                  — 生成的 .pb.h/.pb.cc（根目录，gitignore）
3rd/                       — 三方包引导（packages.json、setup 脚本）
build/                     — 构建输出（gitignore）
```
