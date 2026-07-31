#include "network/etcd/etcd_manager.h"
#include "network/http/http_client.h"
#include "log/log.h"

#include "async_simple/coro/FutureAwaiter.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/co_spawn.hpp>

#include "glaze/glaze.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <vector>

namespace
{

static const std::array<int, 256> MakeDecodeTable()
{
    static constexpr char CHARS[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> t{};
    t.fill(-1);
    for (int i = 0; i < 64; i++)
        t[static_cast<unsigned char>(CHARS[i])] = i;
    return t;
}

static const std::array<int, 256> BASE64_DECODE_TABLE = MakeDecodeTable();
static constexpr const char BASE64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64EncodeInternal(const std::string& input)
{
    std::string result;
    int val = 0, valb = -6;
    for (unsigned char c : input)
    {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0)
        {
            result.push_back(BASE64_CHARS[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6)
        result.push_back(BASE64_CHARS[((val << 8) >> (valb + 8)) & 0x3F]);
    while (result.size() % 4)
        result.push_back('=');
    return result;
}

std::string Base64DecodeInternal(const std::string& input)
{
    std::string result;
    int val = 0, valb = -8;
    for (unsigned char c : input)
    {
        if (BASE64_DECODE_TABLE[c] == -1)
            break;
        val = (val << 6) + BASE64_DECODE_TABLE[c];
        valb += 6;
        if (valb >= 0)
        {
            result.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return result;
}

std::string JsonStr(const char* key, const std::string& value)
{
    return std::string("\"") + key + "\":\"" + value + "\"";
}

constexpr int kHttpPostTimeoutBufferSeconds = 5;

struct SyncHttpPostState
{
    std::mutex mutex;
    std::condition_variable cv;
    int http_status{-1};
    std::string body;
    bool done{false};
};

// Minimal structs for Glaze JSON deserialization of etcd responses.
// Fields not listed here are silently ignored via kLaxJsonOpts.
struct GlzKvEntry
{
    std::string value{};
};

struct GlzRangeResponse
{
    std::vector<GlzKvEntry> kvs{};
};

struct GlzLeaseGrantResponse
{
    std::string ID{};
};

static constexpr glz::opts kLaxJsonOpts{.error_on_unknown_keys = false};

int ParseGetValue(const std::string& response_body, std::string& value)
{
    value.clear();
    GlzRangeResponse resp{};
    auto err = glz::read<kLaxJsonOpts>(resp, response_body);
    if (err)
        return gb::EtcdError::DecodeFailed;
    if (resp.kvs.empty())
        return gb::EtcdError::NotFound;
    value = Base64DecodeInternal(resp.kvs[0].value);
    return gb::EtcdError::OK;
}

int ParseLeaseId(const std::string& response_body, int64_t& lease_id)
{
    lease_id = 0;
    GlzLeaseGrantResponse resp{};
    auto err = glz::read<kLaxJsonOpts>(resp, response_body);
    if (err)
        return gb::EtcdError::DecodeFailed;
    if (resp.ID.empty())
        return gb::EtcdError::DecodeFailed;
    const char* begin = resp.ID.data();
    const char* end   = begin + resp.ID.size();
    auto [ptr, ec]    = std::from_chars(begin, end, lease_id);
    if (ec != std::errc() || ptr != end)
        return gb::EtcdError::DecodeFailed;
    if (lease_id <= 0)
        return gb::EtcdError::DecodeFailed;
    return gb::EtcdError::OK;
}

}

NAMESPACE_BEGIN(gb)

EtcdManager::EtcdManager() = default;

EtcdManager::~EtcdManager()
{
    Disconnect();
}

std::string EtcdManager::Base64Encode(const std::string& input)
{
    return Base64EncodeInternal(input);
}

std::string EtcdManager::Base64Decode(const std::string& input)
{
    return Base64DecodeInternal(input);
}

std::string EtcdManager::MakeUrl(const std::string& path) const
{
    std::string url = endpoint_;
    if (!url.empty() && url.back() != '/')
        url.push_back('/');
    if (!path.empty() && path.front() == '/')
        url.append(path, 1, std::string::npos);
    else
        url.append(path);
    return url;
}

void EtcdManager::EnsureHttpThread()
{
    std::lock_guard<std::mutex> lock(http_mutex_);
    if (http_ioc_)
        return;

    http_ioc_ = std::make_unique<boost::asio::io_context>();
    auto work_guard = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(*http_ioc_));
    http_client_ = std::make_shared<HttpClient>(*http_ioc_);

    http_thread_ = std::thread([this, wg = std::move(work_guard)]() {
        http_ioc_->run();
    });
}

void EtcdManager::StopHttpThread()
{
    std::shared_ptr<HttpClient> client;
    std::unique_ptr<boost::asio::io_context> ioc;
    {
        std::lock_guard<std::mutex> lock(http_mutex_);
        client.swap(http_client_);
        ioc.swap(http_ioc_);
    }

    if (ioc)
        ioc->stop();

    if (http_thread_.joinable())
        http_thread_.join();

    client.reset();
    ioc.reset();
}

int EtcdManager::Connect(const std::string& endpoint)
{
    if (endpoint.empty())
    {
        LOG_ERROR("[EtcdManager] invalid endpoint: {}", endpoint);
        return EtcdError::InvalidArgument;
    }

    endpoint_ = endpoint;
    if (endpoint_.back() != '/')
        endpoint_.push_back('/');

    EnsureHttpThread();
    connected_.store(true, std::memory_order_release);
    LOG_INFO("[EtcdManager] connected endpoint={}", endpoint_);
    return EtcdError::OK;
}

void EtcdManager::Disconnect()
{
    {
        std::lock_guard<std::mutex> lock(watches_mutex_);
        watches_.clear();
    }
    StopHttpThread();
    connected_.store(false, std::memory_order_release);
}

std::string EtcdManager::HttpPost(const std::string& path, const std::string& json_body, int& http_status)
{
    auto state = std::make_shared<SyncHttpPostState>();
    std::chrono::seconds wait_timeout{1};

    {
        std::lock_guard<std::mutex> lock(http_mutex_);
        if (!http_client_)
        {
            http_status = -1;
            return {};
        }

        const auto& timeouts = http_client_->GetTimeouts();
        const int total_timeout_seconds = std::max(
            1,
            timeouts.connect_timeout_seconds +
                timeouts.read_timeout_seconds +
                timeouts.write_timeout_seconds +
                kHttpPostTimeoutBufferSeconds);
        wait_timeout = std::chrono::seconds(total_timeout_seconds);

        std::string url = MakeUrl(path);
        http_client_->Post(url, json_body,
            [state](HttpResponse resp) {
                std::lock_guard<std::mutex> lk(state->mutex);
                state->http_status = resp.status;
                state->body = std::move(resp.body);
                state->done = true;
                state->cv.notify_one();
            },
            "application/json");
    }

    {
        std::unique_lock<std::mutex> lk(state->mutex);
        if (!state->cv.wait_for(lk, wait_timeout, [state] { return state->done; }))
        {
            http_status = -1;
            LOG_ERROR("[EtcdManager] http post timed out, path={} timeout_ms={}",
                      path,
                      std::chrono::duration_cast<std::chrono::milliseconds>(wait_timeout).count());
            return {};
        }
    }

    http_status = state->http_status;
    return state->body;
}

int EtcdManager::Put(const std::string& key, const std::string& value)
{
    return PutWithLease(key, value, 0);
}

int EtcdManager::PutWithLease(const std::string& key, const std::string& value, int64_t lease_id)
{
    if (key.empty())
        return EtcdError::InvalidArgument;
    if (!connected_.load(std::memory_order_acquire))
        return EtcdError::RequestFailed;

    std::string body = "{" + JsonStr("key", Base64Encode(key))
                     + "," + JsonStr("value", Base64Encode(value));
    if (lease_id > 0)
        body += ",\"lease\":\"" + std::to_string(lease_id) + "\"";
    body += "}";

    int http_status = 0;
    std::string resp = HttpPost("/v3/kv/put", body, http_status);

    if (http_status != 200)
    {
        LOG_ERROR("[EtcdManager] put failed, key={} http_status={} resp={}",
                  key, http_status, resp);
        return EtcdError::RequestFailed;
    }
    return EtcdError::OK;
}

int EtcdManager::PutWithTTL(const std::string& key, const std::string& value, int ttl_seconds)
{
    int64_t lease_id = 0;
    int rc = GrantLease(ttl_seconds, lease_id);
    if (rc != EtcdError::OK)
        return rc;
    return PutWithLease(key, value, lease_id);
}

int EtcdManager::Get(const std::string& key, std::string& value)
{
    if (key.empty())
        return EtcdError::InvalidArgument;
    if (!connected_.load(std::memory_order_acquire))
        return EtcdError::RequestFailed;

    std::string body = "{" + JsonStr("key", Base64Encode(key)) + "}";

    int http_status = 0;
    std::string resp = HttpPost("/v3/kv/range", body, http_status);

    if (http_status != 200)
    {
        LOG_ERROR("[EtcdManager] get failed, key={} http_status={} resp={}",
                  key, http_status, resp);
        return EtcdError::RequestFailed;
    }

    int rc = ParseGetValue(resp, value);
    if (rc == EtcdError::DecodeFailed)
        LOG_ERROR("[EtcdManager] get decode failed, key={} resp={}", key, resp);
    return rc;
}

int EtcdManager::Delete(const std::string& key)
{
    if (key.empty())
        return EtcdError::InvalidArgument;
    if (!connected_.load(std::memory_order_acquire))
        return EtcdError::RequestFailed;

    std::string body = "{" + JsonStr("key", Base64Encode(key)) + "}";

    int http_status = 0;
    std::string resp = HttpPost("/v3/kv/deleterange", body, http_status);

    if (http_status != 200)
    {
        LOG_ERROR("[EtcdManager] delete failed, key={} http_status={} resp={}",
                  key, http_status, resp);
        return EtcdError::RequestFailed;
    }
    return EtcdError::OK;
}

int EtcdManager::GrantLease(int ttl_seconds, int64_t& lease_id)
{
    lease_id = 0;

    if (ttl_seconds <= 0)
        return EtcdError::InvalidArgument;
    if (!connected_.load(std::memory_order_acquire))
        return EtcdError::RequestFailed;

    std::string body = "{\"TTL\":\"" + std::to_string(ttl_seconds) + "\"}";

    int http_status = 0;
    std::string resp = HttpPost("/v3/lease/grant", body, http_status);

    if (http_status != 200)
    {
        LOG_ERROR("[EtcdManager] lease grant failed, ttl={} http_status={} resp={}",
                  ttl_seconds, http_status, resp);
        return EtcdError::RequestFailed;
    }

    int rc = ParseLeaseId(resp, lease_id);
    if (rc != EtcdError::OK)
    {
        LOG_ERROR("[EtcdManager] lease grant decode failed, ttl={} resp={}", ttl_seconds, resp);
        return rc;
    }
    return EtcdError::OK;
}

void EtcdManager::AsyncPut(const std::string& key, const std::string& value, PutCallback callback)
{
    if (key.empty())
    {
        if (callback)
            callback(EtcdError::InvalidArgument);
        return;
    }
    if (!connected_.load(std::memory_order_acquire))
    {
        if (callback)
            callback(EtcdError::RequestFailed);
        return;
    }

    std::string body = "{" + JsonStr("key", Base64Encode(key))
                     + "," + JsonStr("value", Base64Encode(value)) + "}";

    std::string url = MakeUrl("/v3/kv/put");
    std::lock_guard<std::mutex> lock(http_mutex_);
    if (!http_client_)
    {
        if (callback)
            callback(EtcdError::RequestFailed);
        return;
    }
    http_client_->Post(url, body,
        [callback = std::move(callback)](HttpResponse resp) {
            if (callback)
                callback(resp.status == 200 ? EtcdError::OK : EtcdError::RequestFailed);
        },
        "application/json");
}

void EtcdManager::AsyncPutWithLease(const std::string& key, const std::string& value, int64_t lease_id, PutCallback callback)
{
    if (key.empty() || lease_id < 0)
    {
        if (callback)
            callback(EtcdError::InvalidArgument);
        return;
    }
    if (!connected_.load(std::memory_order_acquire))
    {
        if (callback)
            callback(EtcdError::RequestFailed);
        return;
    }

    std::string body = "{" + JsonStr("key", Base64Encode(key))
                     + "," + JsonStr("value", Base64Encode(value))
                     + ",\"lease\":\"" + std::to_string(lease_id) + "\"}";

    std::string url = MakeUrl("/v3/kv/put");
    std::lock_guard<std::mutex> lock(http_mutex_);
    if (!http_client_)
    {
        if (callback)
            callback(EtcdError::RequestFailed);
        return;
    }
    http_client_->Post(url, body,
        [callback = std::move(callback)](HttpResponse resp) {
            if (callback)
                callback(resp.status == 200 ? EtcdError::OK : EtcdError::RequestFailed);
        },
        "application/json");
}

void EtcdManager::AsyncPutWithTTL(const std::string& key, const std::string& value, int ttl_seconds, PutCallback callback)
{
    if (key.empty() || ttl_seconds <= 0)
    {
        if (callback)
            callback(EtcdError::InvalidArgument);
        return;
    }
    if (!connected_.load(std::memory_order_acquire))
    {
        if (callback)
            callback(EtcdError::RequestFailed);
        return;
    }

    AsyncGrantLease(ttl_seconds, [this, key, value, callback = std::move(callback)](int rc, int64_t lease_id) mutable {
        if (rc != EtcdError::OK)
        {
            if (callback)
                callback(rc);
            return;
        }
        AsyncPutWithLease(key, value, lease_id, std::move(callback));
    });
}

void EtcdManager::AsyncGet(const std::string& key, GetCallback callback)
{
    if (key.empty())
    {
        if (callback)
            callback(EtcdError::InvalidArgument, {});
        return;
    }
    if (!connected_.load(std::memory_order_acquire))
    {
        if (callback)
            callback(EtcdError::RequestFailed, {});
        return;
    }

    std::string body = "{" + JsonStr("key", Base64Encode(key)) + "}";

    std::string url = MakeUrl("/v3/kv/range");
    std::lock_guard<std::mutex> lock(http_mutex_);
    if (!http_client_)
    {
        if (callback)
            callback(EtcdError::RequestFailed, {});
        return;
    }
    http_client_->Post(url, body,
        [callback = std::move(callback)](HttpResponse resp) {
            if (resp.status != 200)
            {
                if (callback)
                    callback(EtcdError::RequestFailed, {});
                return;
            }

            if (callback)
            {
                std::string value;
                int rc = ParseGetValue(resp.body, value);
                if (rc == EtcdError::DecodeFailed)
                    LOG_ERROR("[EtcdManager] async get decode failed, resp={}", resp.body);
                callback(rc, std::move(value));
            }
        },
        "application/json");
}

void EtcdManager::AsyncDelete(const std::string& key, PutCallback callback)
{
    if (key.empty())
    {
        if (callback)
            callback(EtcdError::InvalidArgument);
        return;
    }
    if (!connected_.load(std::memory_order_acquire))
    {
        if (callback)
            callback(EtcdError::RequestFailed);
        return;
    }

    std::string body = "{" + JsonStr("key", Base64Encode(key)) + "}";

    std::string url = MakeUrl("/v3/kv/deleterange");
    std::lock_guard<std::mutex> lock(http_mutex_);
    if (!http_client_)
    {
        if (callback)
            callback(EtcdError::RequestFailed);
        return;
    }
    http_client_->Post(url, body,
        [callback = std::move(callback)](HttpResponse resp) {
            if (callback)
                callback(resp.status == 200 ? EtcdError::OK : EtcdError::RequestFailed);
        },
        "application/json");
}

void EtcdManager::AsyncGrantLease(int ttl_seconds, LeaseCallback callback)
{
    if (ttl_seconds <= 0)
    {
        if (callback)
            callback(EtcdError::InvalidArgument, 0);
        return;
    }
    if (!connected_.load(std::memory_order_acquire))
    {
        if (callback)
            callback(EtcdError::RequestFailed, 0);
        return;
    }

    std::string body = "{\"TTL\":\"" + std::to_string(ttl_seconds) + "\"}";

    std::string url = MakeUrl("/v3/lease/grant");
    std::lock_guard<std::mutex> lock(http_mutex_);
    if (!http_client_)
    {
        if (callback)
            callback(EtcdError::RequestFailed, 0);
        return;
    }
    http_client_->Post(url, body,
        [callback = std::move(callback)](HttpResponse resp) {
            if (resp.status != 200)
            {
                if (callback)
                    callback(EtcdError::RequestFailed, 0);
                return;
            }

            if (callback)
            {
                int64_t lease_id = 0;
                int rc = ParseLeaseId(resp.body, lease_id);
                if (rc == EtcdError::DecodeFailed)
                    LOG_ERROR("[EtcdManager] async lease grant decode failed, resp={}", resp.body);
                callback(rc, lease_id);
            }
        },
        "application/json");
}

async_simple::coro::Lazy<EtcdResult<void>> EtcdManager::CoPut(const std::string& key, const std::string& value)
{
    async_simple::Promise<EtcdResult<void>> promise;
    auto future = promise.getFuture();
    AsyncPut(key, value, [promise = std::move(promise)](int rc) mutable {
        EtcdResult<void> result;
        result.error_code = rc;
        promise.setValue(std::move(result));
    });
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<EtcdResult<void>> EtcdManager::CoPutWithLease(const std::string& key, const std::string& value, int64_t lease_id)
{
    async_simple::Promise<EtcdResult<void>> promise;
    auto future = promise.getFuture();
    AsyncPutWithLease(key, value, lease_id, [promise = std::move(promise)](int rc) mutable {
        EtcdResult<void> result;
        result.error_code = rc;
        promise.setValue(std::move(result));
    });
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<EtcdResult<void>> EtcdManager::CoPutWithTTL(const std::string& key, const std::string& value, int ttl_seconds)
{
    async_simple::Promise<EtcdResult<void>> promise;
    auto future = promise.getFuture();
    AsyncPutWithTTL(key, value, ttl_seconds, [promise = std::move(promise)](int rc) mutable {
        EtcdResult<void> result;
        result.error_code = rc;
        promise.setValue(std::move(result));
    });
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<EtcdResult<std::string>> EtcdManager::CoGet(const std::string& key)
{
    async_simple::Promise<EtcdResult<std::string>> promise;
    auto future = promise.getFuture();
    AsyncGet(key, [promise = std::move(promise)](int rc, std::string value) mutable {
        EtcdResult<std::string> result;
        result.error_code = rc;
        result.value = std::move(value);
        promise.setValue(std::move(result));
    });
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<EtcdResult<void>> EtcdManager::CoDelete(const std::string& key)
{
    async_simple::Promise<EtcdResult<void>> promise;
    auto future = promise.getFuture();
    AsyncDelete(key, [promise = std::move(promise)](int rc) mutable {
        EtcdResult<void> result;
        result.error_code = rc;
        promise.setValue(std::move(result));
    });
    co_return co_await std::move(future);
}

async_simple::coro::Lazy<EtcdResult<int64_t>> EtcdManager::CoGrantLease(int ttl_seconds)
{
    async_simple::Promise<EtcdResult<int64_t>> promise;
    auto future = promise.getFuture();
    AsyncGrantLease(ttl_seconds, [promise = std::move(promise)](int rc, int64_t lease_id) mutable {
        EtcdResult<int64_t> result;
        result.error_code = rc;
        result.value = lease_id;
        promise.setValue(std::move(result));
    });
    co_return co_await std::move(future);
}

int64_t EtcdManager::NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int EtcdManager::Watch(const std::string& key, WatchCallback callback, int interval_ms)
{
    if (!connected_.load(std::memory_order_acquire))
        return EtcdError::RequestFailed;
    if (key.empty() || !callback)
        return EtcdError::InvalidArgument;
    if (interval_ms < 100)
        interval_ms = 100;

    auto entry = std::make_shared<WatchEntry>();
    entry->watch_id = next_watch_id_.fetch_add(1, std::memory_order_relaxed);
    entry->key = key;
    entry->callback = std::move(callback);
    entry->interval_ms = interval_ms;
    entry->last_poll_ms = NowMs();

    {
        std::lock_guard<std::mutex> lock(watches_mutex_);
        watches_.push_back(entry);
    }

    LOG_INFO("[EtcdManager] watch registered, id={} key={} interval_ms={}",
             entry->watch_id, entry->key, entry->interval_ms);
    return entry->watch_id;
}

int EtcdManager::Unwatch(int watch_id)
{
    std::lock_guard<std::mutex> lock(watches_mutex_);
    auto it = std::find_if(watches_.begin(), watches_.end(),
        [watch_id](const std::shared_ptr<WatchEntry>& e) { return e->watch_id == watch_id; });
    if (it == watches_.end())
        return EtcdError::NotFound;

    LOG_INFO("[EtcdManager] watch removed, id={}", watch_id);
    watches_.erase(it);
    return EtcdError::OK;
}

void EtcdManager::Update()
{
    if (!connected_.load(std::memory_order_acquire))
        return;

    int64_t now = NowMs();

    std::vector<std::shared_ptr<WatchEntry>> snapshot;
    {
        std::lock_guard<std::mutex> lock(watches_mutex_);
        snapshot = watches_;
    }

    for (auto& entry : snapshot)
    {
        if (now - entry->last_poll_ms < entry->interval_ms)
            continue;
        entry->last_poll_ms = now;

        std::string value;
        int rc = Get(entry->key, value);

        if (rc == EtcdError::OK)
        {
            if (!entry->initialized)
            {
                entry->initialized = true;
                entry->has_value = true;
                entry->last_value = value;
            }
            else if (!entry->has_value || entry->last_value != value)
            {
                entry->has_value = true;
                entry->last_value = value;
                entry->callback(entry->key, value, false);
            }
        }
        else if (rc == EtcdError::NotFound)
        {
            if (!entry->initialized)
            {
                entry->initialized = true;
                entry->has_value = false;
                entry->last_value.clear();
            }
            else if (entry->has_value)
            {
                entry->has_value = false;
                entry->last_value.clear();
                entry->callback(entry->key, "", true);
            }
        }
    }
}

NAMESPACE_END
