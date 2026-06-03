#pragma once
#include <cstdint>

namespace gb
{

/// 业务 Worker 类型 —— 用于 RouteTable 和 Worker 自身的类型标记。
/// 通过 MessageType % 10000 映射（旧路由）或显式绑定（实体路由）。
enum ServiceWorkerType : uint8_t
{
    SWT_Normal     = 0,
    SWT_AI         = 1,
    SWT_Navigation = 2,
    SWT_Count
};

} // namespace gb
