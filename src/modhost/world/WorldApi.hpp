#pragma once

#include "modapi/world_api.h"

#include <cstdint>

namespace engine::modhost {

// 查单格：返回 kBlockNotLoaded / kBlockAir / kBlockOccupied，
// 或 kWorldNotReady（世界未 attach）/ kWorldBadArgs。
std::int32_t getBlockImpl(void* host, const modapi::ModBlockPos* pos, std::uint16_t* outId);

// 遍历闭区间内的非空气方块：返回 kWorldOk / kWorldNotReady / kWorldAabbTooLarge / kWorldBadArgs。
// visitor 在引擎解锁之后逐条调用，回调里可以安全地再调引擎查询。
std::int32_t forEachBlockInAABBImpl(void* host,
                                    const modapi::ModBlockPos* min,
                                    const modapi::ModBlockPos* max,
                                    modapi::ModBlockVisitor visitor,
                                    void* user);

// 读世界概况：返回 kWorldOk / kWorldNotReady / kWorldBadArgs。
std::int32_t getWorldInfoImpl(void* host, modapi::ModWorldInfo* out);

} // namespace engine::modhost
