#pragma once

#include "define/def.h"
#include "base/singleton.h"
#include "async_simple/Promise.h"
#include "async_simple/coro/Lazy.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace boost::asio
{
class io_context;
}

namespace gb
{
class HttpClient;
}

NAMESPACE_BEGIN(gb)

namespace EtcdError
{
    constexpr int OK = 0;
    constexpr int RequestFailed = -1;
    constexpr int NotFound = -2;
    constexpr int InvalidArgument = -3;
    constexpr int DecodeFailed = -4;
}

template<typename T>
struct EtcdResult
{
    int error_code{EtcdError::OK};
    T   value{};
};

template<>
struct EtcdResult<void>
{
    int error_code{EtcdError::OK};
};

class EtcdManager : public Singleton<EtcdManager>
{
public:
    using WatchCallback = std::function<void(const std::string& key, const std::string& value, bool deleted)>;

    EtcdManager();
    ~EtcdManager();

    EtcdManager(const EtcdManager&) = delete;
    EtcdManager& operator=(const EtcdManager&) = delete;

    /// Connect to etcd via HTTP API endpoint, e.g. "http://127.0.0.1:2379"
    int Connect(const std::string& endpoint);
    void Disconnect();
    bool IsConnected() const { return connected_.load(std::memory_order_acquire); }

    int Put(const std::string& key, const std::string& value);
    int PutWithLease(const std::string& key, const std::string& value, int64_t lease_id);
    int PutWithTTL(const std::string& key, const std::string& value, int ttl_seconds);
    int Get(const std::string& key, std::string& value);
    int Delete(const std::string& key);
    int GrantLease(int ttl_seconds, int64_t& lease_id);
    /// Register a polling watch implemented by periodic Get() requests.
    /// This is not etcd server-side /v3/watch streaming.
    int Watch(const std::string& key, WatchCallback callback, int interval_ms = 1000);
    int Unwatch(int watch_id);

    /// Call once per frame on the main thread to drive polling watches.
    void Update();

    using PutCallback = std::function<void(int)>;
    using GetCallback = std::function<void(int, std::string)>;
    using LeaseCallback = std::function<void(int, int64_t)>;

    void AsyncPut(const std::string& key, const std::string& value, PutCallback callback);
    void AsyncPutWithLease(const std::string& key, const std::string& value, int64_t lease_id, PutCallback callback);
    void AsyncPutWithTTL(const std::string& key, const std::string& value, int ttl_seconds, PutCallback callback);
    void AsyncGet(const std::string& key, GetCallback callback);
    void AsyncDelete(const std::string& key, PutCallback callback);
    void AsyncGrantLease(int ttl_seconds, LeaseCallback callback);

    async_simple::coro::Lazy<EtcdResult<void>> CoPut(const std::string& key, const std::string& value);
    async_simple::coro::Lazy<EtcdResult<void>> CoPutWithLease(const std::string& key, const std::string& value, int64_t lease_id);
    async_simple::coro::Lazy<EtcdResult<void>> CoPutWithTTL(const std::string& key, const std::string& value, int ttl_seconds);
    async_simple::coro::Lazy<EtcdResult<std::string>> CoGet(const std::string& key);
    async_simple::coro::Lazy<EtcdResult<void>> CoDelete(const std::string& key);
    async_simple::coro::Lazy<EtcdResult<int64_t>> CoGrantLease(int ttl_seconds);

private:
    struct WatchEntry
    {
        int watch_id{0};
        std::string key;
        WatchCallback callback;
        int interval_ms{1000};
        int64_t last_poll_ms{0};
        bool initialized{false};
        bool has_value{false};
        std::string last_value;
    };

    std::string HttpPost(const std::string& path, const std::string& json_body, int& http_status);

    static std::string Base64Encode(const std::string& input);
    static std::string Base64Decode(const std::string& input);

    std::string MakeUrl(const std::string& path) const;

    void EnsureHttpThread();
    void StopHttpThread();

    static int64_t NowMs();

private:
    std::string endpoint_;

    std::unique_ptr<boost::asio::io_context> http_ioc_;
    std::shared_ptr<HttpClient> http_client_;
    std::thread http_thread_;
    std::mutex http_mutex_;

    std::mutex watches_mutex_;
    std::vector<std::shared_ptr<WatchEntry>> watches_;
    std::atomic<int> next_watch_id_{1};
    std::atomic<bool> connected_{false};
};

NAMESPACE_END
