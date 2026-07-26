#pragma once
#include <memory>
#include "../base/atomic.h"

#ifndef TRAN_BUF_BLOCK_BASE_SIZE
// 基础块大小 = 64B
#define TRAN_BUF_BLOCK_BASE_SIZE (64u)
#endif

#ifndef TRAN_BUF_BLOCK_MAX_FACTOR
// 最���块大小 = 64<<9 = 32K
#define TRAN_BUF_BLOCK_MAX_FACTOR (9u)
#endif

/// <summary>
/// 线程安全的传输缓冲区内存池
/// 支持引用计数用于写时复制语义
/// int和atomic<int>大小相同
/// 块布局：BlockSize(int*1) + RefCount(atomic<int>) + Data
/// </summary>


namespace gb
{


class TranBufPool
{
public:
    inline static void* malloc(int factor = 0)
    {
        void* p = ::malloc(TRAN_BUF_BLOCK_BASE_SIZE << factor);
        if (p != nullptr)
        {
            *(reinterpret_cast<int*>(p)) = TRAN_BUF_BLOCK_BASE_SIZE << factor; // 在开头存储块大小
            //*(reinterpret_cast<int*>(p) + 1) = 1;                                      // 初始化引用计数为1
            auto refCount = reinterpret_cast<std::atomic<int>*>(p) + 1;
            *refCount     = 1;
            p             = reinterpret_cast<int*>(p) + 2; // 跳过两个int大小的字段，返回数据指针
        }
        return p;
    }


    inline static int block_size(void* p)
    {
        return *(reinterpret_cast<int*>(p) - 2);
    }

    inline static int capacity(void* p)
    {
        // 块开头的总大小 - 两个int大小的元数据字段
        return *(reinterpret_cast<int*>(p) - 2) - sizeof(int) * 2;
    }

    inline static void add_ref(void* p)
    {
        auto refCount = reinterpret_cast<std::atomic<int>*>(p) - 1;
        //refCount->fetch_add(1, std::memory_order_relaxed);
        atomic_inc(refCount);
    }

    inline static void free(void* p)
    {
        auto refCount = reinterpret_cast<std::atomic<int>*>(p) - 1;
        if (atomic_dec_ret_old(refCount) == 1)
        {
            ::free(reinterpret_cast<int*>(p) - 2);
        }
        //if (atomic_dec_ret_old(reinterpret_cast<int*>(p) - 1) == 1)
        //{
        //    ::free(reinterpret_cast<int*>(p) - 2);
        //}
    }
};

} // namespace gb
