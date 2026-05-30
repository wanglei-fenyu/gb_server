#pragma once 
#include "gbnet/buffer/compressed_def.h"
#include <cstdint>

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