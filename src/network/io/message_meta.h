#pragma once
#include "gbnet/buffer/compressed_def.h"
#include <cstdint>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include "log/log.h"

namespace gb
{

enum MsgMode : uint8_t
{
    Msg      = 0, // 普通消息
    Request  = 1,
    Response = 2,
};

/// ═══════════════════════════════════════════════════════════════════════
/// Meta — 消息头
///
/// user_unique_id 是用户/玩家标识（玩家 ID / NPC ID），完整 64 位。
/// scene_id 供场景路由使用（0 表示不使用场景路由，回退旧行为）。
///
/// 使用 #pragma pack(1) 确保紧凑布局（1字节对齐）
/// ═══════════════════════════════════════════════════════════════════════
#pragma pack(push, 1)
struct Meta
{
    MsgMode      mode{Msg};         // 消息模式 msg rpc rpc回复
    uint64_t     user_unique_id{0}; // 用户/玩家唯一标识（上下文）
    uint32_t     type{0};           // 消息类型
    uint32_t     scene_id{0};       // 场景 ID（路由主键，0=不使用场景路由）
    CompressType compress_type{CompressTypeNone};   // 压缩类型
    uint64_t     method{0};         // rpc方法
    uint64_t     sequence{0};       // rpc序列号
};
#pragma pack(pop)




inline bool ReadMeta(google::protobuf::io::ZeroCopyInputStream* input, Meta& meta, size_t meta_size) {
    const void* data = nullptr;
    int size = 0;

    // 获取数据块
    input->Next(&data, &size);

    std::memcpy(&meta, data, meta_size);

    // 退回未使用的数据
    if (size > static_cast<int>(meta_size))
    {
        input->BackUp(size - static_cast<int>(meta_size));
    }

    return true;
}


}