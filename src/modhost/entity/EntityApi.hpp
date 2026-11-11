#pragma once

#include "modapi/entity_api.h"

#include <cstdint>

namespace engine::modhost {

// 模组注册实体类型：分配类型 id、写入实体类型表，并记录来源。
// host：加载上下文（模组回传的 void*），用来取下一个可用 id 与当前模组名
// def ：模组给出的实体类型定义
// 返回：分配的类型 id（>=0）；拒绝时为 -1
std::int32_t registerEntityImpl(void* host, const modapi::ModEntityDef* def);

} // namespace engine::modhost
