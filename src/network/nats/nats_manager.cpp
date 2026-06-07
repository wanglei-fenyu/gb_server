#include "network/nats/nats_manager.h"
#include "log/log.h"
#include "worker/worker_manager.h"
#include "worker/worker.h"
#include "timer/timer_manager.h"
#include <nats/nats.h>
#include <async_simple/coro/FutureAwaiter.h>
#include <cstring>
#include <memory>
#include <string>

NAMESPACE_BEGIN(gb)

// ── Payload helpers ──────────────────────────────────────────────────

namespace detail
{

inline std::vector<uint8_t> BuildPayload(const Meta& meta,
                                          const std::vector<uint8_t>& body)
{
    std::vector<uint8_t> payload(sizeof(meta));
    std::memcpy(payload.data(), &meta, sizeof(meta));
    if (!body.empty())
        payload.insert(payload.end(), body.begin(), body.end());
    return payload;
}

} // namespace detail

// ── Wildcard match ───────────────────────────────────────────────────

static bool NatsWildcardMatch(const char* pattern, const char* subject)
{
    if (pattern[0] == '>' && pattern[1] == '\0')
        return true;

    const char* p = pattern;
    const char* s = subject;

    while (*p && *s)
    {
        if (*p == '*')
        {
            ++p;
            while (*s && *s != '.')
                ++s;
        }
        else if (*p == '>')
        {
            return true;
        }
        else if (*p == *s)
        {
            ++p;
            ++s;
        }
        else
        {
            return false;
        }
    }

    if (*p == '\0' && *s == '\0')
        return true;
    if (*p == '>' && p[1] == '\0')
        return true;

    return false;
}

static bool MatchSub(const std::string& pattern, const std::string& subject)
{
    if (pattern == subject)
        return true;
    return NatsWildcardMatch(pattern.c_str(), subject.c_str());
}

// ── thread_local per-worker context ──────────────────────────────────

struct TLS_Nats
{
    // Persistent subscriptions: subject → handler
    std::unordered_map<std::string, NatsHandler> subs;

    // Pending request-response: inbox → promise + timer context
    struct PendingRequest
    {
        std::shared_ptr<async_simple::Promise<
            NatsResult<std::vector<uint8_t>>>> promise;
        natsSubscription* nats_sub{nullptr};
        int64_t           timer_id{-1};
    };
    std::unordered_map<std::string, PendingRequest> pending;

    // Inbox counter (per-worker, no atomics needed — single-threaded access)
    uint64_t inbox_seq{0};
};

inline TLS_Nats& tlsNats()
{
    static thread_local TLS_Nats tls;
    return tls;
}

// ── Inbox helpers ────────────────────────────────────────────────────

bool NatsManager::IsNatsInbox(const std::string& subject)
{
    return subject.size() > 6 && subject[0] == '_' &&
           subject.compare(0, 6, "_INBOX") == 0;
}

uint32_t NatsManager::ParseInboxWorkerIndex(const std::string& inbox)
{
    // Format: _INBOX.W{idx}.N{seq}
    auto wpos = inbox.find(".W");
    if (wpos == std::string::npos) return 0;
    wpos += 2; // skip ".W"
    auto npos = inbox.find(".N", wpos);
    if (npos == std::string::npos) return 0;
    return static_cast<uint32_t>(
        std::stoul(inbox.substr(wpos, npos - wpos)));
}

std::string NatsManager::GenInbox()
{
    auto& tls = tlsNats();
    uint32_t wi = 0;
    auto cur = WorkerManager::Instance()->GetCurWorker();
    if (cur)
        wi = cur->GetIndex();
    uint64_t seq = tls.inbox_seq++;
    return "_INBOX.W" + std::to_string(wi) + ".N" + std::to_string(seq);
}

// ── Lifecycle ────────────────────────────────────────────────────────

NatsManager::NatsManager() = default;

NatsManager::~NatsManager()
{
    Disconnect();
}

int NatsManager::Connect(const std::string& url)
{
    if (connected_.load(std::memory_order_acquire))
    {
        LOG_WARN("[NatsManager] already connected");
        return 0;
    }

    if (natsOptions_Create(&opts_) != NATS_OK)
    {
        LOG_ERROR("[NatsManager] failed to create options");
        return -1;
    }
    if (natsOptions_SetURL(opts_, url.c_str()) != NATS_OK)
    {
        LOG_ERROR("[NatsManager] failed to set URL: {}", url);
        natsOptions_Destroy(opts_);
        opts_ = nullptr;
        return -1;
    }

    natsStatus s = natsConnection_Connect(&conn_, opts_);
    if (s != NATS_OK)
    {
        LOG_ERROR("[NatsManager] failed to connect to {}: {}",
                  url, natsStatus_GetText(s));
        natsOptions_Destroy(opts_);
        opts_  = nullptr;
        conn_ = nullptr;
        return -1;
    }

    connected_.store(true, std::memory_order_release);
    LOG_INFO("[NatsManager] connected to {}", url);
    return 0;
}

void NatsManager::Disconnect()
{
    if (!connected_.load(std::memory_order_acquire))
        return;

    connected_.store(false, std::memory_order_release);

    // Close the nats connection.  This stops all internal I/O threads and
    // auto-unsubscribes every subscription.  No more callbacks will fire
    // after this returns (nats.c guarantee).
    if (conn_)
    {
        natsConnection_Close(conn_);
        conn_ = nullptr;
    }
    if (opts_)
    {
        natsOptions_Destroy(opts_);
        opts_ = nullptr;
    }

    // Free all NatsSubCtx allocations — no more callbacks can fire.
    {
        std::lock_guard<std::mutex> lock(sub_ctx_mutex_);
        sub_ctxs_.clear();
    }

    LOG_INFO("[NatsManager] disconnected");
}

// ── Publish ──────────────────────────────────────────────────────────

void NatsManager::Publish(const std::string&       subject,
                          const Meta&              meta,
                          const std::vector<uint8_t>& data)
{
    if (!connected_.load(std::memory_order_acquire))
    {
        LOG_WARN("[NatsManager] Publish while disconnected, dropping");
        return;
    }

    auto payload = detail::BuildPayload(meta, data);

    natsStatus s = natsConnection_Publish(
        conn_, subject.c_str(),
        payload.data(), static_cast<int>(payload.size()));

    if (s != NATS_OK)
        LOG_ERROR("[NatsManager] Publish '{}' failed: {}", subject,
                  natsStatus_GetText(s));
}

void NatsManager::Publish(const std::string&       subject,
                           const Meta&              meta,
                           const google::protobuf::Message& msg)
{
    if (!connected_.load(std::memory_order_acquire))
    {
        LOG_WARN("[NatsManager] Publish while disconnected, dropping");
        return;
    }

    std::string proto_data;
    if (!msg.SerializeToString(&proto_data))
    {
        LOG_ERROR("[NatsManager] Publish proto serialization failed");
        return;
    }
    std::vector<uint8_t> body(proto_data.begin(), proto_data.end());
    auto payload = detail::BuildPayload(meta, body);

    natsStatus s = natsConnection_Publish(
        conn_, subject.c_str(),
        payload.data(), static_cast<int>(payload.size()));

    if (s != NATS_OK)
        LOG_ERROR("[NatsManager] Publish proto '{}' failed: {}", subject,
                  natsStatus_GetText(s));
}

// ── Subscribe ────────────────────────────────────────────────────────
//
// Store the handler in the current worker's thread_local subscription
// map, then register the NATS subscription.  The nats.c callback
// (OnNatsSubMsg) posts back to this worker so the actual handler
// runs on the same thread that subscribed — no locks needed for the
// subs lookup on the hot path.

void NatsManager::Subscribe(const std::string& subject, NatsHandler handler)
{
    if (!connected_.load(std::memory_order_acquire))
    {
        LOG_ERROR("[NatsManager] Subscribe before Connect");
        return;
    }

    // Capture current worker index for the per-subscription closure
    uint32_t wi = 0;
    auto cur = WorkerManager::Instance()->GetCurWorker();
    if (cur)
        wi = cur->GetIndex();

    // Store handler in this worker's thread_local storage (no lock)
    tlsNats().subs[subject] = std::move(handler);

    // Allocate per-subscription context that nats.c will pass back to
    // OnNatsSubMsg.  Freed in Disconnect() after the connection closes.
    auto ctx = std::make_unique<NatsSubCtx>();
    ctx->worker_index = wi;

    natsSubscription* nats_sub = nullptr;
    natsStatus s = natsConnection_Subscribe(
        &nats_sub, conn_, subject.c_str(),
        &NatsManager::OnNatsSubMsg, ctx.get());

    if (s != NATS_OK)
    {
        LOG_ERROR("[NatsManager] Subscribe '{}' failed: {}", subject,
                  natsStatus_GetText(s));
        tlsNats().subs.erase(subject);
        return;
    }

    // Track the context so Disconnect can free it
    {
        std::lock_guard<std::mutex> lock(sub_ctx_mutex_);
        sub_ctxs_.push_back(std::move(ctx));
    }

    LOG_INFO("[NatsManager] subscribed to '{}' (worker {})", subject, wi);
}

// ── RequestRaw (coroutine) ──────────────────────────────────────────
//
// 1. Generate inbox name that embeds the current worker_index.
// 2. Subscribe to the inbox (OnNatsInboxMsg parses worker_index from
//    the inbox name — no per-subscription closure needed).
// 3. Store the promise in thread_local pending map.
// 4. PublishRequest with inbox as reply subject.
// 5. Register timeout on this worker's TimerManager.
//
// Both the response handler (posted to worker queue by OnNatsInboxMsg)
// and the timeout timer (fired by the worker's TimerManager::Update)
// execute on the same worker thread → they run sequentially, so the
// second one always finds the pending entry already removed.  No
// atomic done flag required.

async_simple::coro::Lazy<NatsResult<std::vector<uint8_t>>>
NatsManager::RequestRaw(const std::string&          subject,
                        const Meta&                 meta,
                        const std::vector<uint8_t>& data,
                        std::chrono::milliseconds   timeout)
{
    if (!connected_.load(std::memory_order_acquire))
    {
        LOG_WARN("[NatsManager] Request while disconnected");
        co_return NatsResult<std::vector<uint8_t>>{NatsError::Disconnected};
    }

    // ── 1. Unique inbox with embedded worker_index ────────────────
    std::string inbox = GenInbox();

    // ── 2. Subscribe to inbox ─────────────────────────────────────
    natsSubscription* nats_sub = nullptr;
    natsStatus subStatus = natsConnection_Subscribe(
        &nats_sub, conn_, inbox.c_str(),
        &NatsManager::OnNatsInboxMsg, nullptr);
    if (subStatus != NATS_OK)
    {
        LOG_ERROR("[NatsManager] inbox subscribe failed: {}",
                  natsStatus_GetText(subStatus));
        co_return NatsResult<std::vector<uint8_t>>{NatsError::SubscribeFailed};
    }
    natsSubscription_AutoUnsubscribe(nats_sub, 1);

    // ── 3. Promise → thread_local pending map ─────────────────────
    auto promise = std::make_shared<
        async_simple::Promise<NatsResult<std::vector<uint8_t>>>>();
    auto future = promise->getFuture();

    {
        auto& tls = tlsNats();
        TLS_Nats::PendingRequest pr;
        pr.promise   = promise;
        pr.nats_sub = nats_sub;
        tls.pending[inbox] = std::move(pr);
    }

    // ── 4. PublishRequest ─────────────────────────────────────────
    auto payload = detail::BuildPayload(meta, data);
    natsStatus pubStatus = natsConnection_PublishRequest(
        conn_, subject.c_str(), inbox.c_str(),
        payload.data(), static_cast<int>(payload.size()));
    if (pubStatus != NATS_OK)
    {
        LOG_ERROR("[NatsManager] PublishRequest failed: {}",
                  natsStatus_GetText(pubStatus));
        // Clean up — same worker thread, no races
        auto& tls = tlsNats();
        auto it = tls.pending.find(inbox);
        if (it != tls.pending.end())
        {
            natsSubscription_Destroy(it->second.nats_sub);
            tls.pending.erase(it);
        }
        co_return NatsResult<std::vector<uint8_t>>{NatsError::RequestFailed};
    }

    // ── 5. Timeout timer on the requesting worker's TimerManager ──
    auto cur = WorkerManager::Instance()->GetCurWorker();
    if (cur)
    {
        int64_t timer_id = cur->GetTimerManager()->RegisterTimer(
            timeout,
            [inbox]() {
                auto& tls = tlsNats();
                auto it = tls.pending.find(inbox);
                if (it == tls.pending.end())
                    return;  // response already handled

                // Unsubscribe + destroy subscription
                natsSubscription_Unsubscribe(it->second.nats_sub);
                natsSubscription_Destroy(it->second.nats_sub);

                // Fulfill with Timeout error
                auto promise = std::move(it->second.promise);
                tls.pending.erase(it);
                promise->setValue(
                    NatsResult<std::vector<uint8_t>>{NatsError::Timeout});
            },
            false /* one-shot */);

        // Store timer_id in the pending entry
        auto& tls = tlsNats();
        auto it = tls.pending.find(inbox);
        if (it != tls.pending.end())
            it->second.timer_id = timer_id;
    }

    // ── 6. Await ──────────────────────────────────────────────────
    auto result = co_await std::move(future);
    co_return result;
}

// ── Reply ────────────────────────────────────────────────────────────

void NatsManager::Reply(const std::string&       reply_to,
                        const Meta&              meta,
                        const std::vector<uint8_t>& data)
{
    if (!connected_.load(std::memory_order_acquire))
    {
        LOG_WARN("[NatsManager] Reply while disconnected");
        return;
    }

    auto payload = detail::BuildPayload(meta, data);
    natsStatus s = natsConnection_Publish(
        conn_, reply_to.c_str(),
        payload.data(), static_cast<int>(payload.size()));

    if (s != NATS_OK)
        LOG_ERROR("[NatsManager] Reply to '{}' failed: {}", reply_to,
                  natsStatus_GetText(s));
}

void NatsManager::Reply(const std::string&       reply_to,
                         const Meta&              meta,
                         const google::protobuf::Message& msg)
{
    if (!connected_.load(std::memory_order_acquire))
    {
        LOG_WARN("[NatsManager] Reply while disconnected");
        return;
    }

    std::string proto_data;
    if (!msg.SerializeToString(&proto_data))
    {
        LOG_ERROR("[NatsManager] Reply proto serialization failed");
        return;
    }
    std::vector<uint8_t> body(proto_data.begin(), proto_data.end());
    auto payload = detail::BuildPayload(meta, body);

    natsStatus s = natsConnection_Publish(
        conn_, reply_to.c_str(),
        payload.data(), static_cast<int>(payload.size()));

    if (s != NATS_OK)
        LOG_ERROR("[NatsManager] Reply proto to '{}' failed: {}", reply_to,
                  natsStatus_GetText(s));
}

// ══════════════════════════════════════════════════════════════════════
// nats.c callbacks  (fire on nats.c's internal I/O threads)
// ══════════════════════════════════════════════════════════════════════

// ── OnNatsSubMsg ─────────────────────────────────────────────────────
// For persistent subscriptions created via Subscribe().
// The closure is a NatsSubCtx* holding the subscribing worker's index.

void NatsManager::OnNatsSubMsg(natsConnection*  /*nc*/,
                               natsSubscription* /*sub*/,
                               natsMsg*         msg,
                               void*            closure)
{
    auto* ctx = static_cast<NatsSubCtx*>(closure);
    uint32_t worker_index = ctx->worker_index;

    // Extract all data before destroying the natsMsg
    const char* subject_cstr = natsMsg_GetSubject(msg);
    if (!subject_cstr)
    {
        natsMsg_Destroy(msg);
        return;
    }
    std::string subject = subject_cstr;

    const char* data_cstr = natsMsg_GetData(msg);
    int         data_len  = natsMsg_GetDataLength(msg);
    std::vector<uint8_t> payload;
    if (data_cstr && data_len > 0)
        payload.assign(
            reinterpret_cast<const uint8_t*>(data_cstr),
            reinterpret_cast<const uint8_t*>(data_cstr) + data_len);

    const char* reply_cstr = natsMsg_GetReply(msg);
    std::string reply_to = reply_cstr ? reply_cstr : "";

    natsMsg_Destroy(msg);

    // Route to the subscribing worker thread
    WorkerManager::Instance()->PostToWorker(
        static_cast<size_t>(worker_index),
        [subject, reply_to, payload]() {
            auto& tls = tlsNats();
            auto it = tls.subs.find(subject);
            if (it != tls.subs.end())
            {
                Meta meta{};
                std::vector<uint8_t> body;
                if (payload.size() >= sizeof(Meta))
                {
                    std::memcpy(&meta, payload.data(), sizeof(Meta));
                    body.assign(payload.begin() + sizeof(Meta), payload.end());
                }
                it->second(meta, body, reply_to);
                return;
            }

            // Wildcard fallback
            for (auto& kv : tls.subs)
            {
                if (MatchSub(kv.first, subject))
                {
                    Meta meta{};
                    std::vector<uint8_t> body;
                    if (payload.size() >= sizeof(Meta))
                    {
                        std::memcpy(&meta, payload.data(), sizeof(Meta));
                        body.assign(payload.begin() + sizeof(Meta), payload.end());
                    }
                    kv.second(meta, body, reply_to);
                    return;
                }
            }

            LOG_DEBUG("[NatsManager] no handler for '{}'", subject);
        });
}

// ── OnNatsInboxMsg ───────────────────────────────────────────────────
// For inbox subscriptions created by RequestRaw.
// The worker_index is extracted from the inbox name itself, so the
// closure parameter is unused.

void NatsManager::OnNatsInboxMsg(natsConnection*  /*nc*/,
                                 natsSubscription* /*sub*/,
                                 natsMsg*         msg,
                                 void*            /*closure*/)
{
    // Extract inbox (the subject IS the inbox name)
    const char* subject_cstr = natsMsg_GetSubject(msg);
    if (!subject_cstr)
    {
        natsMsg_Destroy(msg);
        return;
    }
    std::string inbox = subject_cstr;

    // Parse the originating worker from the inbox name
    uint32_t worker_index = ParseInboxWorkerIndex(inbox);

    // Extract payload before destroying msg
    const char* data_cstr = natsMsg_GetData(msg);
    int         data_len  = natsMsg_GetDataLength(msg);
    std::vector<uint8_t> payload;
    if (data_cstr && data_len > 0)
        payload.assign(
            reinterpret_cast<const uint8_t*>(data_cstr),
            reinterpret_cast<const uint8_t*>(data_cstr) + data_len);

    natsMsg_Destroy(msg);

    // Route back to the requesting worker thread
    WorkerManager::Instance()->PostToWorker(
        static_cast<size_t>(worker_index),
        [inbox, payload]() {
            auto& tls = tlsNats();
            auto it = tls.pending.find(inbox);
            if (it == tls.pending.end())
                return;  // request already timed out

            // Cancel the timeout timer
            if (it->second.timer_id >= 0)
            {
                auto cur = WorkerManager::Instance()->GetCurWorker();
                if (cur)
                    cur->GetTimerManager()->UnRegisterTimer(
                        it->second.timer_id);
            }

            // Destroy subscription (nats.c already auto-unsubscribed,
            // this just frees the natsSubscription resources)
            if (it->second.nats_sub)
            {
                natsSubscription_Destroy(it->second.nats_sub);
                it->second.nats_sub = nullptr;
            }

            // Parse Meta + body
            Meta meta{};
            std::vector<uint8_t> body;
            if (payload.size() >= sizeof(Meta))
            {
                std::memcpy(&meta, payload.data(), sizeof(Meta));
                body.assign(payload.begin() + sizeof(Meta), payload.end());
            }

            // Fulfill the promise
            auto promise = std::move(it->second.promise);
            tls.pending.erase(it);
            promise->setValue(
                NatsResult<std::vector<uint8_t>>{
                    NatsError::OK, std::move(body)});
        });
}

NAMESPACE_END
