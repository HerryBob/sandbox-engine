#pragma once

#include "modapi/block_api.h"

#include <cstdint>

namespace engine::modhost {

// 模组注册方块：分配 id、写入方块注册表，并记录来源。
// host：加载上下文（模组回传的 void*），用来取下一个可用 id 与当前模组名
// def ：模组给出的方块定义
// 返回：分配的 id（>=1）；拒绝时为 -1
std::int32_t registerBlockImpl(void* host, const modapi::ModBlockDef* def);

} // namespace engine::modhost
