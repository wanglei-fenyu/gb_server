#pragma once
#include "base/singleton.h"
#include "msgpack/msgpack.hpp"
#include "network/io/message_meta.h"
#include <async_simple/Promise.h>
#include <async_simple/coro/Lazy.h>
#include <google/protobuf/message.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <tuple>
#include <type_traits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct __natsConnection;
struct __natsOptions;
struct __natsSubscription;
struct __natsMsg;
typedef struct __natsConnection natsConnection;
typedef struct __natsOptions natsOptions;
typedef struct __natsSubscription natsSubscription;
typedef struct __natsMsg natsMsg;

NAMESPACE_BEGIN(gb)

// ── Error codes ───────────────────────────────────────────────────────

namespace NatsError {
    constexpr int OK              = 0;
    constexpr int Timeout         = -1;
    constexpr int Disconnected    = -2;
    constexpr int PublishFailed   = -3;
    constexpr int SubscribeFailed = -4;
    constexpr int RequestFailed   = -5;
}

// ── NatsResult ────────────────────────────────────────────────────────
// NatsResult<T>          — single value: .value is T
// NatsResult<T1,T2,...>  — multiple msgpack values: .value is std::tuple<T1,T2,...>
// NatsResult<void>       — status only, no value

template<typename... Args>
struct NatsResult
{
    int error_code{NatsError::OK};
    using ValueType = std::conditional_t<(sizeof...(Args) == 1),
        std::tuple_element_t<0, std::tuple<Args...>>,
        std::tuple<Args...>>;
    ValueType value{};
};

template<>
struct NatsResult<void>
{
    int error_code{NatsError::OK};
};

// ── Handler ──────────────────────────────────────────────────────────
// handler(meta, body_bytes, reply_to)
// reply_to is non-empty when the incoming message is a Request.

using NatsHandler = std::function<void(
    const Meta&, const std::vector<uint8_t>&, const std::string&)>;

// ── Per-subscription context (nats.c closure) ────────────────────────

struct NatsSubCtx
{
    uint32_t    worker_index{0};
};

// ── NATS Manager ─────────────────────────────────────────────────────

class NatsManager : public Singleton<NatsManager>
{
public:
    NatsManager();
    ~NatsManager();

    NatsManager(const NatsManager&) = delete;
    NatsManager& operator=(const NatsManager&) = delete;

    // ── Connection ──────────────────────────────────────────────────

    int  Connect(const std::string& url);
    void Disconnect();
    bool IsConnected() const { return connected_.load(std::memory_order_acquire); }

    // ── Publish ─────────────────────────────────────────────────────

    void Publish(const std::string&       subject,
                 const Meta&              meta,
                 const std::vector<uint8_t>& data = {});

    void Publish(const std::string&       subject,
                 const Meta&              meta,
                 const google::protobuf::Message& msg);

    // Variadic Publish — msgpack-packs all extra arguments into the body
    // Disabled for single protobuf argument (delegates to proto overload).
    template<typename... Args,
        typename = std::enable_if_t<!(sizeof...(Args) == 1 &&
            std::is_base_of_v<google::protobuf::Message,
                std::decay_t<std::tuple_element_t<0, std::tuple<Args...>>>>)>>
    void Publish(const std::string& subject,
                 const Meta&        meta,
                 Args&&...          args)
    {
        auto data = gb::msgpack::pack(
            std::make_tuple(std::forward<Args>(args)...));
        Publish(subject, meta, data);
    }

    // ── Subscribe ───────────────────────────────────────────────────
    // handler is stored per-worker via thread_local.
    // nats.c callback delivers to the subscribing worker's event queue.

    void Subscribe(const std::string& subject, NatsHandler handler);

    // Generalized Subscribe template — variadic, supports multi-value msgpack:
    //   Subscribe<MyProto>("sub", [](NatsResult<MyProto> r) { ... });        // protobuf
    //   Subscribe<int>("sub", [](NatsResult<int> r) { ... });                // single msgpack
    //   Subscribe<int,float>("sub", [](NatsResult<int,float> r) {            // multi msgpack
    //       auto [a, b] = r.value;
    //   });
    template<typename... Args>
    void Subscribe(const std::string& subject,
                   std::function<void(NatsResult<Args...>)> handler)
    {
        Subscribe(subject, [handler = std::move(handler)](
            const Meta& /*meta*/, const std::vector<uint8_t>& body, const std::string& /*reply_to*/)
        {
            if constexpr (sizeof...(Args) == 1)
            {
                using T = std::tuple_element_t<0, std::tuple<Args...>>;
                if constexpr (std::is_base_of_v<google::protobuf::Message, T>)
                {
                    T msg;
                    if (!msg.ParseFromArray(body.data(), static_cast<int>(body.size())))
                    {
                        handler(NatsResult<Args...>{NatsError::RequestFailed});
                        return;
                    }
                    handler(NatsResult<Args...>{NatsError::OK, std::move(msg)});
                }
                else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
                {
                    handler(NatsResult<Args...>{NatsError::OK, body});
                }
                else
                {
                    auto tuple = gb::msgpack::unpack<T>(body);
                    handler(NatsResult<Args...>{NatsError::OK, std::get<0>(std::move(tuple))});
                }
            }
            else
            {
                // Multi-value: msgpack unpack into tuple
                using TupleType = std::tuple<Args...>;
                auto tuple = gb::msgpack::unpack<TupleType>(body);
                handler(NatsResult<Args...>{NatsError::OK, std::get<0>(std::move(tuple))});
            }
        });
    }

    // ── Request (coroutine only) ────────────────────────────────────

    async_simple::coro::Lazy<NatsResult<std::vector<uint8_t>>> RequestRaw(
        const std::string&          subject,
        const Meta&                 meta,
        const std::vector<uint8_t>& data,
        std::chrono::milliseconds   timeout = std::chrono::seconds(5));

    template<typename T>
    async_simple::coro::Lazy<NatsResult<T>> Request(
        const std::string&          subject,
        const Meta&                 meta,
        const std::vector<uint8_t>& data,
        std::chrono::milliseconds   timeout = std::chrono::seconds(5))
    {
        auto raw = co_await RequestRaw(subject, meta, data, timeout);
        if (raw.error_code != NatsError::OK)
        {
            if constexpr (std::is_void_v<T>)
                co_return NatsResult<T>{raw.error_code};
            else
                co_return NatsResult<T>{raw.error_code};
        }
        if constexpr (std::is_void_v<T>)
        {
            co_return NatsResult<T>{NatsError::OK};
        }
        else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
        {
            co_return NatsResult<T>{NatsError::OK, std::move(raw.value)};
        }
        else
        {
            auto tuple = gb::msgpack::unpack<T>(raw.value);
            co_return NatsResult<T>{NatsError::OK, std::get<0>(std::move(tuple))};
        }
    }

    // ── Request with protobuf request / response ────────────────────
    //   auto result = co_await NatsManager::Instance()->Request<MyProto>(
    //       "sub", meta, request_msg);
    //   result.value is MyProto by value.

    template<typename ProtoMsg>
    async_simple::coro::Lazy<NatsResult<ProtoMsg>> Request(
        const std::string&          subject,
        const Meta&                 meta,
        const google::protobuf::Message& request,
        std::chrono::milliseconds   timeout = std::chrono::seconds(5))
    {
        std::string proto_data;
        if (!request.SerializeToString(&proto_data))
        {
            co_return NatsResult<ProtoMsg>{NatsError::RequestFailed};
        }
        std::vector<uint8_t> data(proto_data.begin(), proto_data.end());

        auto raw = co_await RequestRaw(subject, meta, data, timeout);
        if (raw.error_code != NatsError::OK)
        {
            co_return NatsResult<ProtoMsg>{raw.error_code};
        }

        ProtoMsg response;
        if (!response.ParseFromArray(raw.value.data(),
                                     static_cast<int>(raw.value.size())))
        {
            co_return NatsResult<ProtoMsg>{NatsError::RequestFailed};
        }

        co_return NatsResult<ProtoMsg>{NatsError::OK, std::move(response)};
    }

    // Variadic Request — msgpack-pack request args, msgpack-unpack response.
    //   auto r = co_await Request<int,float>("sub", meta, arg1, arg2);
    //   r.value is tuple<int,float> when Ret pack size > 1, else T directly.
    // SFINAE: disabled when Args is exactly one vector<uint8_t> or one
    // protobuf message (delegates to the existing raw / proto overloads).
    template<typename... Ret, typename... Args,
        typename = std::enable_if_t<
            (sizeof...(Args) > 1) ||
            (sizeof...(Args) == 1 && !std::is_same_v<
                std::decay_t<std::tuple_element_t<0, std::tuple<Args...>>>,
                std::vector<uint8_t>> &&
                !std::is_base_of_v<google::protobuf::Message,
                    std::decay_t<std::tuple_element_t<0, std::tuple<Args...>>>>)>>
    async_simple::coro::Lazy<NatsResult<Ret...>> Request(
        const std::string& subject,
        const Meta&        meta,
        Args&&...          args)
    {
        auto data = gb::msgpack::pack(
            std::make_tuple(std::forward<Args>(args)...));
        auto raw = co_await RequestRaw(subject, meta, data);
        if (raw.error_code != NatsError::OK)
            co_return NatsResult<Ret...>{raw.error_code};

        if constexpr (sizeof...(Ret) == 0)
        {
            co_return NatsResult<Ret...>{NatsError::OK};
        }
        else if constexpr (sizeof...(Ret) == 1)
        {
            using T = std::tuple_element_t<0, std::tuple<Ret...>>;
            auto tuple = gb::msgpack::unpack<T>(raw.value);
            co_return NatsResult<Ret...>{NatsError::OK, std::get<0>(std::move(tuple))};
        }
        else
        {
            using TupleType = std::tuple<Ret...>;
            auto tuple = gb::msgpack::unpack<TupleType>(raw.value);
            co_return NatsResult<Ret...>{NatsError::OK, std::get<0>(std::move(tuple))};
        }
    }

    // ── Reply ───────────────────────────────────────────────────────

    void Reply(const std::string&       reply_to,
               const Meta&              meta,
               const std::vector<uint8_t>& data = {});

    void Reply(const std::string&       reply_to,
               const Meta&              meta,
               const google::protobuf::Message& msg);

    // Variadic Reply — msgpack-packs all extra arguments into the body
    // Disabled for single protobuf argument (delegates to proto overload).
    template<typename... Args,
        typename = std::enable_if_t<!(sizeof...(Args) == 1 &&
            std::is_base_of_v<google::protobuf::Message,
                std::decay_t<std::tuple_element_t<0, std::tuple<Args...>>>>)>>
    void Reply(const std::string& reply_to,
               const Meta&        meta,
               Args&&...          args)
    {
        auto data = gb::msgpack::pack(
            std::make_tuple(std::forward<Args>(args)...));
        Reply(reply_to, meta, data);
    }

private:
    // nats.c callbacks (static — fire on nats.c I/O threads)
    static void OnNatsSubMsg(natsConnection* nc, natsSubscription* sub,
                             natsMsg* msg, void* closure);
    static void OnNatsInboxMsg(natsConnection* nc, natsSubscription* sub,
                               natsMsg* msg, void* closure);

    // Internals
    std::string     GenInbox();
    static bool     IsNatsInbox(const std::string& subject);
    static uint32_t ParseInboxWorkerIndex(const std::string& inbox);

private:
    natsConnection*   conn_{nullptr};
    natsOptions*      opts_{nullptr};
    std::atomic<bool> connected_{false};

    // NatsSubCtx tracking (for cleanup on Disconnect)
    std::mutex                                sub_ctx_mutex_;
    std::vector<std::unique_ptr<NatsSubCtx>>  sub_ctxs_;
};

NAMESPACE_END
