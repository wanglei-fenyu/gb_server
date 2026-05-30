#pragma once

#include "rpc_call.h"
#include "rpc_function.hpp"
#include <unordered_map>

namespace gb {

// ============================================================================
// Thread-Local RPC 注册表和调用栈
// ============================================================================

class ThreadLocalRpcContext {
public:
    // 获取当前线程的上下文
    static ThreadLocalRpcContext& Current();

    // ========== 注册阶段（仅初始化时调用） ==========
    
    /// 在当前线程注册 RPC 处理器
    /// 应该在服务器启动期、同一线程内调用
    void RegisterRpcFunction(uint64_t method_hash, rpc_listen_fun fn);

    /// 冻结此线程的 RPC 注册表，之后只能读不能写
    void FreezeRpc();

    // ========== 运行期查询（零锁） ==========

    /// 查找已注册的 RPC 处理器，返回 nullptr 表示未找到
    const rpc_listen_fun* FindRpcFunction(uint64_t method_hash) const;

    // ========== RPC 调用追踪（零锁） ==========

    /// 记录待处理的 RPC 调用（在 RPC 发起时调用）
    /// 返回 true 表示插入成功，false 表示已存在（错误）
    bool InsertRpcCall(uint64_t seq_id, const RpcCallPtr& call);

    /// 删除已完成的 RPC 调用
    /// 返回对应的 RpcCallPtr，如果不存在返回 nullptr
    RpcCallPtr RemoveRpcCall(uint64_t seq_id);

    /// 查找待处理的 RPC 调用（不删除）
    RpcCallPtr FindRpcCall(uint64_t seq_id) const;

    /// 获取所有待处理的 RPC 调用（用于清理/超时检测）
    std::vector<RpcCallPtr> GetAllRpcCalls() const;

    /// 清空所有待处理 RPC 调用
    void ClearAllRpcCalls();

private:
    ThreadLocalRpcContext() = default;
    ~ThreadLocalRpcContext() = default;

    // 禁止拷贝和移动
    ThreadLocalRpcContext(const ThreadLocalRpcContext&) = delete;
    ThreadLocalRpcContext& operator=(const ThreadLocalRpcContext&) = delete;
    ThreadLocalRpcContext(ThreadLocalRpcContext&&) = delete;
    ThreadLocalRpcContext& operator=(ThreadLocalRpcContext&&) = delete;

private:
    // RPC 方法哈希值 -> 处理函数
    // 初始化完后不再修改，只读访问（无锁）
    std::unordered_map<uint64_t, rpc_listen_fun> rpc_interface_map_;

    // RPC sequence id -> RpcCall (正在等待的调用)
    // 仅本线程访问，无需锁
    std::unordered_map<uint64_t, RpcCallPtr> rpc_caller_map_;

    bool frozen_{false};
};

// ============================================================================
// 全局维护：用于初始化时收集所有线程的 RPC 注册
// ============================================================================

/// 注册所有线程的 RPC（在所有 Worker 启动后调用）
/// 这个函数遍历所有 Worker，在各自线程内注册 RPC
void InitializeAllThreadLocalRpc();

/// 冻结所有线程的 RPC 注册表（启动完成后调用）
void FreezeAllThreadLocalRpc();

} // namespace gb
