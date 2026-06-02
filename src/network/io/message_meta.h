#pragma once 
#include "gbnet/buffer/compressed_def.h"
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace gb
{

enum MsgMode : uint8_t
{
    Msg      = 0, // 普通消息
    Request  = 1,
    Response = 2,
};


struct Meta
{
    MsgMode      mode{Msg};         // 消息模式 msg rpc rpc回复
    uint64_t      id{0};            // 用户唯一标识
    uint32_t      type{0};          // 消息类型
    CompressType compress_type{CompressTypeNone};   // 压缩类型
    uint64_t      method{0};        // rpc方法
    uint64_t      sequence{0};      // rpc序列号
};


// 从输入流读取 meta_size 字节填充 Meta。
// 流可能将数据切成多个块返回（分片），因此需要跨块累积拷贝，
// 并对每个块校验 Next() 返回值，避免越界读取。
inline bool ReadMeta(google::protobuf::io::ZeroCopyInputStream* input, Meta& meta, size_t meta_size)
{
    auto*  dst    = reinterpret_cast<uint8_t*>(&meta);
    size_t copied = 0;

    while (copied < meta_size)
    {
        const void* data = nullptr;
        int         size = 0;
        if (!input->Next(&data, &size))
            return false;
        if (size <= 0)
            continue;

        size_t take = std::min(static_cast<size_t>(size), meta_size - copied);
        std::memcpy(dst + copied, data, take);
        copied += take;

        // 退回当前块中未使用的数据
        if (static_cast<size_t>(size) > take)
            input->BackUp(size - static_cast<int>(take));
    }

    return true;
}


}
