#pragma once
#include <cstdint>
#include <vector>
#include <atomic>
#include <algorithm>
#include <memory>
#include "log/log.h"

namespace gb {

/// 无锁路由表 —— 双缓冲 + 排序向量 + 二分查找
///
/// 职责
///   将 entity_id (uint64_t) 映射到目标 Worker 索引。
///   读端（IO / Worker 线程）：load atomic → 二分查找，无锁、无等待。
///   写端（主线程独占）：修改 pending_，Freeze() 时排序 → 原子 swap。
///
/// 约束
///   区间不可重叠（entity_id 唯一属于一个 Worker）。
///   若 Bind 的区间与已有区间重叠，则覆盖重叠部分。
///   Bind(单例) → 区间 [entity_id, entity_id+1)。
///
class LockFreeRouteTable
{
public:
    static constexpr uint32_t kInvalidWorker = UINT32_MAX;

    struct Entry
    {
        uint64_t begin{0};          // inclusive
        uint64_t end{0};            // exclusive
        uint32_t worker_index{kInvalidWorker};
    };

    using Table = std::vector<Entry>;

public:
    LockFreeRouteTable()
        : pending_(new Table())
        , retired_(nullptr)
    {
        current_.store(nullptr, std::memory_order_release);
    }

    ~LockFreeRouteTable()
    {
        delete current_.load(std::memory_order_relaxed);
        delete pending_;
        delete retired_;
    }

    // ── 写端（必须在主线程调用） ──────────────────────────────

    /// 绑定单个 entity_id 到指定 Worker。
    void BindSingle(uint64_t entity_id, uint32_t worker_index)
    {
        Bind(entity_id, entity_id + 1, worker_index);
    }

    /// 绑定区间 [entity_begin, entity_end) 到指定 Worker。
    /// 与已有区间重叠的部分将被覆盖。
    void Bind(uint64_t entity_begin, uint64_t entity_end, uint32_t worker_index)
    {
        if (entity_begin >= entity_end)
            return;

        // 移除被本区间覆盖的已有条目
        auto& vec = *pending_;
        for (auto it = vec.begin(); it != vec.end(); )
        {
            if (it->end <= entity_begin || it->begin >= entity_end)
            {
                // 不重叠
                ++it;
                continue;
            }

            if (it->begin < entity_begin && it->end > entity_end)
            {
                // 完全包围：拆分为左右两段
                // 必须先保存右段结束位置，it->end 随后会被修改
                uint64_t right_end  = it->end;
                uint64_t left_end   = entity_begin;
                uint64_t right_begin = entity_end;
                it->end = left_end;
                vec.insert(it + 1, Entry{right_begin, right_end, it->worker_index});
                // 重新调整 iter（插入后 it 可能失效）
                // 简化：直接跳出，外部 for 下次从头再扫
                break;
            }

            if (it->begin < entity_begin)
            {
                // 左截断
                it->end = entity_begin;
                ++it;
            }
            else if (it->end > entity_end)
            {
                // 右截断
                it->begin = entity_end;
                ++it;
            }
            else
            {
                // 完全包含在新区间内 → 删除
                it = vec.erase(it);
            }
        }

        vec.push_back(Entry{entity_begin, entity_end, worker_index});
    }

    /// 解除单例 entity_id 的绑定（移除包含此 ID 的区间）
    void Unbind(uint64_t entity_id)
    {
        auto& vec = *pending_;
        for (auto it = vec.begin(); it != vec.end(); ++it)
        {
            if (entity_id >= it->begin && entity_id < it->end)
            {
                if (it->begin == entity_id && it->end == entity_id + 1)
                {
                    vec.erase(it);
                }
                else if (it->begin == entity_id)
                {
                    it->begin = entity_id + 1;
                }
                else if (it->end == entity_id + 1)
                {
                    it->end = entity_id;
                }
                else
                {
                    // 在中间：拆分成两段
                    uint64_t right_begin = entity_id + 1;
                    uint64_t right_end   = it->end;
                    it->end = entity_id;
                    vec.insert(it + 1, Entry{right_begin, right_end, it->worker_index});
                }
                break;
            }
        }
    }

    /// 冻结：排序 pending_ → 创建只读快照 → 原子 swap → 回收旧快照
    void Freeze()
    {
        // 排序 & 合并重叠
        std::sort(pending_->begin(), pending_->end(),
            [](const Entry& a, const Entry& b) { return a.begin < b.begin; });

        auto* snapshot = new Table();
        for (auto& e : *pending_)
        {
            if (!snapshot->empty() && snapshot->back().end > e.begin)
            {
                // 合并重叠（取最新的 worker_index）
                if (snapshot->back().end < e.end)
                    snapshot->back().end = e.end;
            }
            else
            {
                snapshot->push_back(e);
            }
        }

        // 原子 swap
        auto* old = current_.exchange(snapshot, std::memory_order_acq_rel);

        // 延迟回收：retired_ 保证上一个快照在两次 swap 后安全释放
        delete retired_;
        retired_ = old;
    }

    // ── 读端（任意线程，无锁） ──────────────────────────────────

    /// 查找 entity_id 所属的 Worker 索引。
    /// 返回 kInvalidWorker 表示未找到。
    uint32_t Lookup(uint64_t entity_id) const noexcept
    {
        auto* table = current_.load(std::memory_order_acquire);
        if (!table || table->empty())
            return kInvalidWorker;

        // 二分查找最后一个 begin <= entity_id 的条目
        auto it = std::upper_bound(table->begin(), table->end(), entity_id,
            [](uint64_t val, const Entry& e) { return val < e.begin; });

        if (it == table->begin())
            return kInvalidWorker;

        --it;
        if (entity_id >= it->begin && entity_id < it->end)
            return it->worker_index;

        return kInvalidWorker;
    }

    /// 当前快照是否为空（读端辅助，非线程安全 — 仅用于调试/监控）
    bool IsEmpty() const noexcept
    {
        auto* table = current_.load(std::memory_order_relaxed);
        return !table || table->empty();
    }

private:
    Table*                    pending_;              // 写缓冲区（主线程独占）
    std::atomic<Table*>       current_;              // 只读快照（任意线程读）
    Table*                    retired_;              // 待回收的快照
};

}
