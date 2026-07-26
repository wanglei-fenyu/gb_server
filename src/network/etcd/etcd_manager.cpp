#include "network/etcd/etcd_manager.h"
#include "log/log.h"
#include <etcd/Client.hpp>

#include "async_simple/coro/FutureAwaiter.h"

#include <pplx/pplxtasks.h>

#include <chrono>
#include <exception>

NAMESPACE_BEGIN(gb)

EtcdManager::EtcdManager() = default;

EtcdManager::~EtcdManager()
{
    Disconnect();
}

int EtcdManager::Connect(const std::string& endpoint)
{
    if (endpoint.empty())
    {
        LOG_ERROR("[EtcdManager] invalid endpoint: {}", endpoint);
        return EtcdError::InvalidArgument;
    }

    try
    {
        std::lock_guard<std::mutex> lock(client_mutex_);
        endpoint_ = endpoint;
        client_ = std::make_shared<etcd::Client>(endpoint_);
        connected_.store(true, std::memory_order_release);
        LOG_INFO("[EtcdManager] connected endpoint={}", endpoint_);
        return EtcdError::OK;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[EtcdManager] connect failed, endpoint={} err={}", endpoint, e.what());
        connected_.store(false, std::memory_order_release);
        return EtcdError::RequestFailed;
    }
}

void EtcdManager::Disconnect()
{
    StopAllWatches();
    std::lock_guard<std::mutex> lock(client_mutex_);
    client_.reset();
    connected_.store(false, std::memory_order_release);
}

int EtcdManager::Put(const std::string& key, const std::string& value)
{
    return PutWithLease(key, value, 0);
}

int EtcdManager::PutWithLease(const std::string& key, const std::string& value, int64_t lease_id)
{
    auto client = GetClient();
    if (!client)
        return EtcdError::RequestFailed;

    if (key.empty())
        return EtcdError::InvalidArgument;

    try
    {
        auto response = (lease_id > 0)
            ? client->set(key, value, lease_id).get()
            : client->set(key, value).get();

        if (!response.is_ok())
        {
            LOG_ERROR("[EtcdManager] put failed, key={} code={} msg={}",
                      key, response.error_code(), response.error_message());
            return EtcdError::RequestFailed;
        }
        return EtcdError::OK;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[EtcdManager] put exception, key={} err={}", key, e.what());
        return EtcdError::RequestFailed;
    }
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
    auto client = GetClient();
    if (!client)
        return EtcdError::RequestFailed;

    if (key.empty())
        return EtcdError::InvalidArgument;

    try
    {
        auto response = client->get(key).get();
        if (!response.is_ok())
        {
            LOG_WARN("[EtcdManager] get miss/fail, key={} code={} msg={}",
                     key, response.error_code(), response.error_message());
            return EtcdError::NotFound;
        }

        value = response.value().as_string();
        return EtcdError::OK;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[EtcdManager] get exception, key={} err={}", key, e.what());
        return EtcdError::RequestFailed;
    }
}

int EtcdManager::Delete(const std::string& key)
{
    auto client = GetClient();
    if (!client)
        return EtcdError::RequestFailed;

    if (key.empty())
        return EtcdError::InvalidArgument;

    try
    {
        auto response = client->rm(key).get();
        if (!response.is_ok())
        {
            LOG_ERROR("[EtcdManager] delete failed, key={} code={} msg={}",
                      key, response.error_code(), response.error_message());
            return EtcdError::RequestFailed;
        }
        return EtcdError::OK;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[EtcdManager] delete exception, key={} err={}", key, e.what());
        return EtcdError::RequestFailed;
    }
}

int EtcdManager::GrantLease(int ttl_seconds, int64_t& lease_id)
{
    lease_id = 0;

    auto client = GetClient();
    if (!client)
        return EtcdError::RequestFailed;
    if (ttl_seconds <= 0)
        return EtcdError::InvalidArgument;

    try
    {
        auto response = client->leasegrant(ttl_seconds).get();
        if (!response.is_ok())
        {
            LOG_ERROR("[EtcdManager] lease grant failed, ttl={} code={} msg={}",
                      ttl_seconds, response.error_code(), response.error_message());
            return EtcdError::RequestFailed;
        }

        lease_id = response.value().lease();
        return EtcdError::OK;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[EtcdManager] lease grant exception, ttl={} err={}", ttl_seconds, e.what());
        return EtcdError::RequestFailed;
    }
}

int EtcdManager::EnsureClient() const
{
    if (!connected_.load(std::memory_order_acquire))
        return EtcdError::RequestFailed;

    std::lock_guard<std::mutex> lock(client_mutex_);
    if (!client_)
        return EtcdError::RequestFailed;
    return EtcdError::OK;
}

std::shared_ptr<etcd::Client> EtcdManager::GetClient() const
{
    std::lock_guard<std::mutex> lock(client_mutex_);
    return client_;
}

void EtcdManager::AsyncPut(const std::string& key, const std::string& value, PutCallback callback)
{
    if (key.empty())
    {
        if (callback)
            callback(EtcdError::InvalidArgument);
        return;
    }

    auto client = GetClient();
    if (!client)
    {
        if (callback)
            callback(EtcdError::RequestFailed);
        return;
    }

    client->put(key, value).then([callback = std::move(callback)](pplx::task<etcd::Response> task) mutable {
        int rc = EtcdError::RequestFailed;
        try
        {
            auto response = task.get();
            rc = response.error_code();
        }
        catch (const std::exception&)
        {
            rc = EtcdError::RequestFailed;
        }

        if (callback)
            callback(rc);
    });
}

void EtcdManager::AsyncPutWithLease(const std::string& key, const std::string& value, int64_t lease_id, PutCallback callback)
{
    if (key.empty() || lease_id < 0)
    {
        if (callback)
            callback(EtcdError::InvalidArgument);
        return;
    }

    auto client = GetClient();
    if (!client)
    {
        if (callback)
            callback(EtcdError::RequestFailed);
        return;
    }

    client->put(key, value, lease_id).then([callback = std::move(callback)](pplx::task<etcd::Response> task) mutable {
        int rc = EtcdError::RequestFailed;
        try
        {
            rc = task.get().error_code();
        }
        catch (const std::exception&)
        {
            rc = EtcdError::RequestFailed;
        }

        if (callback)
            callback(rc);
    });
}

void EtcdManager::AsyncPutWithTTL(const std::string& key, const std::string& value, int ttl_seconds, PutCallback callback)
{
    if (key.empty() || ttl_seconds <= 0)
    {
        if (callback)
            callback(EtcdError::InvalidArgument);
        return;
    }

    auto client = GetClient();
    if (!client)
    {
        if (callback)
            callback(EtcdError::RequestFailed);
        return;
    }

    client->leasegrant(ttl_seconds).then([this, client, key, value, callback = std::move(callback)](pplx::task<etcd::Response> task) mutable {
        int rc = EtcdError::RequestFailed;
        try
        {
            auto response = task.get();
            if (!response.is_ok())
            {
                rc = response.error_code();
            }
            else
            {
                auto lease_id = response.value().lease();
                client->put(key, value, lease_id).then([callback = std::move(callback)](pplx::task<etcd::Response> put_task) mutable {
                    int put_rc = EtcdError::RequestFailed;
                    try
                    {
                        put_rc = put_task.get().error_code();
                    }
                    catch (const std::exception&)
                    {
                        put_rc = EtcdError::RequestFailed;
                    }
                    if (callback)
                        callback(put_rc);
                });
                return;
            }
        }
        catch (const std::exception&)
        {
            rc = EtcdError::RequestFailed;
        }

        if (callback)
            callback(rc);
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

    auto client = GetClient();
    if (!client)
    {
        if (callback)
            callback(EtcdError::RequestFailed, {});
        return;
    }

    client->get(key).then([callback = std::move(callback)](pplx::task<etcd::Response> task) mutable {
        int rc = EtcdError::RequestFailed;
        std::string value;
        try
        {
            auto response = task.get();
            rc = response.error_code();
            if (response.is_ok())
                value = response.value().as_string();
        }
        catch (const std::exception&)
        {
            rc = EtcdError::RequestFailed;
        }

        if (callback)
            callback(rc, std::move(value));
    });
}

void EtcdManager::AsyncDelete(const std::string& key, PutCallback callback)
{
    if (key.empty())
    {
        if (callback)
            callback(EtcdError::InvalidArgument);
        return;
    }

    auto client = GetClient();
    if (!client)
    {
        if (callback)
            callback(EtcdError::RequestFailed);
        return;
    }

    client->rm(key).then([callback = std::move(callback)](pplx::task<etcd::Response> task) mutable {
        int rc = EtcdError::RequestFailed;
        try
        {
            rc = task.get().error_code();
        }
        catch (const std::exception&)
        {
            rc = EtcdError::RequestFailed;
        }

        if (callback)
            callback(rc);
    });
}

void EtcdManager::AsyncGrantLease(int ttl_seconds, LeaseCallback callback)
{
    if (ttl_seconds <= 0)
    {
        if (callback)
            callback(EtcdError::InvalidArgument, 0);
        return;
    }

    auto client = GetClient();
    if (!client)
    {
        if (callback)
            callback(EtcdError::RequestFailed, 0);
        return;
    }

    client->leasegrant(ttl_seconds).then([callback = std::move(callback)](pplx::task<etcd::Response> task) mutable {
        int rc = EtcdError::RequestFailed;
        int64_t lease_id = 0;
        try
        {
            auto response = task.get();
            rc = response.error_code();
            if (response.is_ok())
                lease_id = response.value().lease();
        }
        catch (const std::exception&)
        {
            rc = EtcdError::RequestFailed;
        }

        if (callback)
            callback(rc, lease_id);
    });
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

int EtcdManager::Watch(const std::string& key, WatchCallback callback, int interval_ms)
{
    if (EnsureClient() != EtcdError::OK)
        return EtcdError::RequestFailed;
    if (key.empty() || !callback)
        return EtcdError::InvalidArgument;
    if (interval_ms < 100)
        interval_ms = 100;

    auto task = std::make_shared<WatchTask>();
    task->watch_id = next_watch_id_.fetch_add(1, std::memory_order_relaxed);
    task->key = key;
    task->callback = std::move(callback);
    task->interval_ms = interval_ms;

    task->thread = std::thread([this, task]() {
        while (!task->stop.load(std::memory_order_acquire))
        {
            std::string value;
            int rc = Get(task->key, value);

            if (rc == EtcdError::OK)
            {
                if (!task->initialized)
                {
                    task->initialized = true;
                    task->has_value = true;
                    task->last_value = value;
                }
                else if (!task->has_value || task->last_value != value)
                {
                    task->has_value = true;
                    task->last_value = value;
                    task->callback(task->key, value, false);
                }
            }
            else if (rc == EtcdError::NotFound)
            {
                if (!task->initialized)
                {
                    task->initialized = true;
                    task->has_value = false;
                    task->last_value.clear();
                }
                else if (task->has_value)
                {
                    task->has_value = false;
                    task->last_value.clear();
                    task->callback(task->key, "", true);
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(task->interval_ms));
        }
    });

    {
        std::lock_guard<std::mutex> lock(watches_mutex_);
        watches_[task->watch_id] = task;
    }

    LOG_INFO("[EtcdManager] watch started, id={} key={} interval_ms={}",
             task->watch_id, task->key, task->interval_ms);
    return task->watch_id;
}

int EtcdManager::Unwatch(int watch_id)
{
    std::shared_ptr<WatchTask> task;
    {
        std::lock_guard<std::mutex> lock(watches_mutex_);
        auto it = watches_.find(watch_id);
        if (it == watches_.end())
            return EtcdError::NotFound;
        task = std::move(it->second);
        watches_.erase(it);
    }

    task->stop.store(true, std::memory_order_release);
    if (task->thread.joinable())
        task->thread.join();

    LOG_INFO("[EtcdManager] watch stopped, id={} key={}", watch_id, task->key);
    return EtcdError::OK;
}

void EtcdManager::StopAllWatches()
{
    std::unordered_map<int, std::shared_ptr<WatchTask>> tasks;
    {
        std::lock_guard<std::mutex> lock(watches_mutex_);
        tasks.swap(watches_);
    }

    for (auto& kv : tasks)
    {
        auto& task = kv.second;
        task->stop.store(true, std::memory_order_release);
    }

    for (auto& kv : tasks)
    {
        auto& task = kv.second;
        if (task->thread.joinable())
            task->thread.join();
    }
}

NAMESPACE_END
