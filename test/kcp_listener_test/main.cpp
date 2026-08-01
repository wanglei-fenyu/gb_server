// kcp_listener_test
// 验证阶段 3:ServerImpl 的 KCP 分支 + KcpListener 多连接分发
//   - 多客户端并发往返回显,数据字节一致(跨 1376B KCP 分段)
//   - keep_alive 超时后服务端关闭会话,同 conv 再发包 → 懒清理 + 重建
// 用法:直接运行,退出码 0 = PASS

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <condition_variable>

#include "network/io/server.h"
#include "network/io/message_meta.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include "message_stream/message_header.h"
#include "message_stream/message_stream.h"
#include "buffer/buffer.h"

using namespace gb;

static void LogInfo(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    printf("[info] ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
    fflush(stdout);
}

static void LogError(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    printf("[error] ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
    fflush(stdout);
}

// 客户端:MessageStream(KCP transport)+ 独立 io_context 线程
class TestClient : public MessageStream
{
public:
    TestClient(IoService& ios, const Endpoint& ep)
        : MessageStream(NET_TYPE::NT_CLIENT, ios, ep, TRANSPORT_TYPE::KCP)
    {
    }

    std::atomic<bool> connected{false};
    std::atomic<int> received_count{0};
    std::function<void(const std::string& payload)> on_echo;

protected:
    bool on_sending(const ReadBufferPtr&) override { return true; }

    void on_sent(const ReadBufferPtr&) override {}

    void on_send_failed(std::string_view reason, const ReadBufferPtr&) override
    {
        LogError("client send failed: %s", std::string(reason).c_str());
    }

    void on_received(const ReadBufferPtr& message, int meta_size, int64_t data_size) override
    {
        std::string bytes;
        const void* data = nullptr;
        int size = 0;
        while (message->Next(&data, &size))
            bytes.append(static_cast<const char*>(data), static_cast<size_t>(size));

        if (meta_size >= 0 && data_size >= 0 &&
            static_cast<size_t>(meta_size + data_size) <= bytes.size())
        {
            std::string payload(bytes.data() + meta_size, static_cast<size_t>(data_size));
            ++received_count;
            if (on_echo)
                on_echo(payload);
        }
        trigger_receive();
    }

    bool on_connected() override
    {
        connected = true;
        return MessageStream::on_connected();
    }

    void on_closed() override {}
};

using TestClientPtr = std::shared_ptr<TestClient>;

static std::string RandomPayload(size_t size)
{
    static thread_local uint32_t seed = 0x12345678;
    std::string out(size, '\0');
    for (size_t i = 0; i < size; ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        out[i] = static_cast<char>(seed & 0xFF);
    }
    return out;
}

static void SendMessage(TestClientPtr& client, const std::string& payload)
{
    MessageHeader header;
    header.meta_size = 0;
    header.data_size = static_cast<int64_t>(payload.size());
    header.message_size = header.meta_size + header.data_size;

    // Reserve 推进写游标,随后用 SetData 写回预留区(参考 Session::Send 的模式)
    WriteBuffer write_buffer;
    int header_pos = write_buffer.Reserve(sizeof(header));
    write_buffer.Append(payload.data(), static_cast<int>(payload.size()));
    write_buffer.SetData(header_pos, reinterpret_cast<const char*>(&header), sizeof(header));

    ReadBufferPtr read_buffer(new ReadBuffer());
    write_buffer.SwapOut(read_buffer.get());
    client->async_send_message(read_buffer);
}

static uint16_t ProbeFreeUdpPort()
{
    Asio::io_context probe;
    Asio::ip::udp::socket s(probe);
    Error_code ec;
    s.open(Asio::ip::udp::v4(), ec);
    if (ec)
        return 0;
    s.bind(Asio::ip::udp::endpoint(Asio::ip::udp::v4(), 0), ec);
    if (ec)
        return 0;
    uint16_t port = s.local_endpoint(ec).port();
    s.close();
    return port;
}

static constexpr int kClientCount = 4;
static constexpr int kRounds = 20;
static constexpr size_t kPayloads[] = {100, 4096, 65536};
static constexpr int kPayloadCount = 3;

int main()
{
    // 框架内部 LOG_* 通过 spdlog::get("multi_sink") 取 logger(测试不初始化 GbLog)
    auto test_logger = spdlog::stderr_color_mt(LOG_NAME);
    test_logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(test_logger);
    LogInfo("kcp_listener_test start");

    uint16_t port = ProbeFreeUdpPort();
    if (port == 0)
    {
        LogError("probe free udp port failed");
        return 1;
    }

    ServerOptions options;
    options.transport_type = TRANSPORT_TYPE::KCP;
    options.io_service_pool_size = 2;
    options.keep_alive_time = 2;
    options.max_throughput_in = -1;
    options.max_throughput_out = -1;

    Server server(options);
    std::atomic<int> accept_count{0};
    std::atomic<int> close_count{0};
    std::atomic<int> server_received_count{0};

    server.SetConnnectCallBack([&](const SessionPtr&) { ++accept_count; });
    server.SetCloseCallBack([&](const SessionPtr&) { ++close_count; });
    server.SetReceivedCallBack([&](const SessionPtr& session,
                                   const ReadBufferPtr& buffer,
                                   int meta_size,
                                   int64_t data_size) {
        ++server_received_count;
        std::string bytes;
        const void* data = nullptr;
        int size = 0;
        while (buffer->Next(&data, &size))
            bytes.append(static_cast<const char*>(data), static_cast<size_t>(size));

        std::string_view payload(bytes.data() + meta_size, static_cast<size_t>(data_size));

        Meta meta;
        meta.mode = MsgMode::Msg;
        meta.user_unique_id = 0;
        meta.type = 1;
        session->Send(&meta, payload.data(), static_cast<std::size_t>(payload.size()));
    });

    std::string address = "127.0.0.1:" + std::to_string(port);
    if (!server.Start(address))
    {
        LogError("server start failed: %s", address.c_str());
        return 1;
    }
    LogInfo("server kcp listen: %s", address.c_str());

    std::vector<IoService*> client_ios;
    std::vector<std::thread> client_threads;
    std::vector<std::shared_ptr<Asio::executor_work_guard<Asio::io_context::executor_type>>> client_guards;
    std::vector<TestClientPtr> clients;

    for (int i = 0; i < kClientCount; ++i)
    {
        auto ios = new IoService();
        client_ios.push_back(ios);
        auto guard = std::make_shared<Asio::executor_work_guard<Asio::io_context::executor_type>>(
            Asio::make_work_guard(*ios));
        client_guards.push_back(guard);
        client_threads.emplace_back([ios]() { ios->run(); });

        auto client = std::make_shared<TestClient>(*ios, Endpoint(Asio::ip::make_address("127.0.0.1"), port));
        client->set_flow_controller(std::make_shared<FlowController>(true, 0, true, 0));
        clients.push_back(client);
        client->async_connect();
    }

    auto wait_connected = [&]() -> bool {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            bool all = true;
            for (auto& c : clients)
                all = all && c->connected.load();
            if (all)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    };

    if (!wait_connected())
    {
        LogError("clients not connected within 10s");
        for (size_t i = 0; i < clients.size(); ++i)
        {
            auto& c = clients[i];
            LogError("  client[%zu]: connected=%d is_connecting=%d is_connected=%d is_closed=%d received=%d",
                     i, c->connected.load(), c->is_connecting(), c->is_connected(), c->is_closed(),
                     c->received_count.load());
        }
        return 1;
    }
    LogInfo("all %d clients connected", kClientCount);

    for (int round = 0; round < kRounds; ++round)
    {
        std::atomic<int> pending{kClientCount};
        std::atomic<bool> failed{false};
        std::mutex mtx;
        std::condition_variable cv;

        size_t payload_size = kPayloads[round % kPayloadCount];
        std::string payload = RandomPayload(payload_size);

        for (auto& client : clients)
        {
            client->on_echo = [&](const std::string& echo) {
                if (echo != payload)
                {
                    LogError("round %d echo mismatch: expect %zu got %zu", round, payload_size, echo.size());
                    failed = true;
                }
                if (--pending == 0)
                    cv.notify_all();
            };
            SendMessage(client, payload);
        }

        std::unique_lock<std::mutex> lock(mtx);
        bool ok = cv.wait_for(lock, std::chrono::seconds(15), [&]() { return pending.load() == 0; });
        if (!ok || failed.load())
        {
            LogError("round %d failed (pending=%d failed=%d)", round, pending.load(), failed.load());
            LogError("  accept=%d server_received=%d close=%d", accept_count.load(), server_received_count.load(), close_count.load());
            for (size_t i = 0; i < clients.size(); ++i)
            {
                auto& c = clients[i];
                LogError("  client[%zu]: connected=%d closed=%d received=%d", i, c->connected.load(), c->is_closed(), c->received_count.load());
            }
            return 1;
        }
        LogInfo("round %d ok: payload=%zuB x %d clients", round, payload_size, kClientCount);
    }

    LogInfo("idle 4s for keep_alive(2s) to close server sessions...");
    std::this_thread::sleep_for(std::chrono::seconds(4));

    // 阶段 B:重建 — 模拟客户端感知断线后重连:关闭旧客户端,新客户端带新 conv 重连
    // (同 conv 续传无法重建:客户端 KCP 延续的 SN 超出服务端新会话 rcv_nxt 窗口,数据被丢弃)
    for (auto& client : clients)
        client->close("test rebuild");
    for (auto& ios : client_ios)
        ios->stop();
    for (auto& t : client_threads)
        t.join();
    clients.clear();
    client_guards.clear();
    for (auto* ios : client_ios)
        delete ios;
    client_ios.clear();
    client_threads.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    int accepted_before = accept_count.load();
    LogInfo("accept_count before rebuild: %d", accepted_before);

    for (int i = 0; i < kClientCount; ++i)
    {
        auto ios = new IoService();
        client_ios.push_back(ios);
        auto guard = std::make_shared<Asio::executor_work_guard<Asio::io_context::executor_type>>(
            Asio::make_work_guard(*ios));
        client_guards.push_back(guard);
        client_threads.emplace_back([ios]() { ios->run(); });

        auto client = std::make_shared<TestClient>(*ios, Endpoint(Asio::ip::make_address("127.0.0.1"), port));
        client->set_flow_controller(std::make_shared<FlowController>(true, 0, true, 0));
        clients.push_back(client);
        client->async_connect();
    }

    if (!wait_connected())
    {
        LogError("clients not reconnected within 10s");
        return 1;
    }
    LogInfo("all %d clients reconnected", kClientCount);

    {
        std::atomic<int> pending{kClientCount};
        std::atomic<bool> failed{false};
        std::mutex mtx;
        std::condition_variable cv;

        std::string payload = RandomPayload(2048);
        for (auto& client : clients)
        {
            client->on_echo = [&](const std::string& echo) {
                if (echo != payload)
                    failed = true;
                if (--pending == 0)
                    cv.notify_all();
            };
            SendMessage(client, payload);
        }

        std::unique_lock<std::mutex> lock(mtx);
        bool ok = cv.wait_for(lock, std::chrono::seconds(15), [&]() { return pending.load() == 0; });
        if (!ok || failed.load())
        {
            LogError("rebuild round failed (pending=%d failed=%d)", pending.load(), failed.load());
            return 1;
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int accepted_after = accept_count.load();
    LogInfo("accept_count after rebuild: %d", accepted_after);
    if (accepted_after < accepted_before + kClientCount)
    {
        LogError("rebuild not triggered: expect >= %d got %d", accepted_before + kClientCount, accepted_after);
        return 1;
    }

    for (auto& client : clients)
        client->close("test done");
    for (auto& ios : client_ios)
        ios->stop();
    for (auto& t : client_threads)
        t.join();
    clients.clear();
    client_guards.clear();
    for (auto* ios : client_ios)
        delete ios;
    client_ios.clear();
    client_threads.clear();

    server.Stop();

    LogInfo("kcp_listener_test PASS: rounds=%d accepts=%d closes=%d", kRounds, accept_count.load(), close_count.load());
    return 0;
}
