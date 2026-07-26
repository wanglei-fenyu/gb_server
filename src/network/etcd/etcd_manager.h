#pragma once

#include "base/singleton.h"
#include "async_simple/Promise.h"
#include "async_simple/coro/Lazy.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>

namespace etcd
{
class Client;
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

    int Connect(const std::string& endpoint);
    void Disconnect();
    bool IsConnected() const { return connected_.load(std::memory_order_acquire); }

    int Put(const std::string& key, const std::string& value);
    int PutWithLease(const std::string& key, const std::string& value, int64_t lease_id);
    int PutWithTTL(const std::string& key, const std::string& value, int ttl_seconds);
    int Get(const std::string& key, std::string& value);
    int Delete(const std::string& key);
    int GrantLease(int ttl_seconds, int64_t& lease_id);
    int Watch(const std::string& key, WatchCallback callback, int interval_ms = 1000);
    int Unwatch(int watch_id);

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
    struct WatchTask
    {
        int watch_id{0};
        std::string key;
        WatchCallback callback;
        int interval_ms{1000};
        std::atomic<bool> stop{false};
        std::thread thread;

        bool initialized{false};
        bool has_value{false};
        std::string last_value;
    };

    int EnsureClient() const;
    std::shared_ptr<etcd::Client> GetClient() const;
    void StopAllWatches();

private:
    std::string endpoint_;
    mutable std::mutex client_mutex_;
    std::shared_ptr<etcd::Client> client_;
    std::mutex watches_mutex_;
    std::unordered_map<int, std::shared_ptr<WatchTask>> watches_;
    std::atomic<int> next_watch_id_{1};
    std::atomic<bool> connected_{false};
};

NAMESPACE_END
