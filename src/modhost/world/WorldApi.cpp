#include "modhost/world/WorldApi.hpp"

#include "business/World.hpp"
#include "modhost/shared/HostContext.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace engine::modhost {

std::int32_t getBlockImpl(void* host, const ModBlockPos* pos, std::uint16_t* outId) {
    if (host == nullptr || pos == nullptr || outId == nullptr) return kWorldBadArgs;
    const HostContext* ctx = static_cast<const HostContext*>(host);
    const engine::business::World* world = ctx->world;
    if (world == nullptr) return kWorldNotReady;

    const engine::shared::BlockPos p{pos->x, pos->y, pos->z};
    engine::shared::BlockType type = engine::shared::BlockType::Air;
    switch (world->lookupBlock(p, type)) {
        case engine::business::World::LookupResult::NotLoaded:
            return kBlockNotLoaded;
        case engine::business::World::LookupResult::Air:
            return kBlockAir;
        case engine::business::World::LookupResult::Occupied:
            *outId = static_cast<std::uint16_t>(type);
            return kBlockOccupied;
    }
    return kBlockNotLoaded;
}

std::int32_t forEachBlockInAABBImpl(void* host,
                                    const ModBlockPos* min,
                                    const ModBlockPos* max,
                                    ModBlockVisitor visitor,
                                    void* user) {
    if (host == nullptr || min == nullptr || max == nullptr || visitor == nullptr)
        return kWorldBadArgs;
    const HostContext* ctx = static_cast<const HostContext*>(host);
    const engine::business::World* world = ctx->world;
    if (world == nullptr) return kWorldNotReady;

    // 体积用 int64 算：三个 int32 轴长直接相乘会溢出。
    const std::int64_t dx = static_cast<std::int64_t>(max->x) - min->x + 1;
    const std::int64_t dy = static_cast<std::int64_t>(max->y) - min->y + 1;
    const std::int64_t dz = static_cast<std::int64_t>(max->z) - min->z + 1;
    if (dx <= 0 || dy <= 0 || dz <= 0) return kWorldBadArgs;
    const std::int64_t volume = dx * dy * dz;
    if (volume > static_cast<std::int64_t>(kMaxAabbVolume))
        return kWorldAabbTooLarge;

    // World::forEachBlockInAABB 是持锁遍历 + 锁内回调，回调里再调 getBlock 会对
    // 同一把不可重入的 mutex 重入死锁。所以锁内只收集，等 World 自行解锁后再逐条回调。
    const std::int64_t reserveHint = (volume < 4096) ? volume : 4096;
    std::vector<std::pair<engine::shared::BlockPos, std::uint16_t>> collected;
    collected.reserve(static_cast<std::size_t>(reserveHint));
    world->forEachBlockInAABB(
        engine::shared::BlockPos{min->x, min->y, min->z},
        engine::shared::BlockPos{max->x, max->y, max->z},
        [&collected](const engine::shared::BlockPos& p, engine::shared::BlockType t) {
            collected.emplace_back(p, static_cast<std::uint16_t>(t));
        });

    for (const auto& item : collected) {
        ModBlockPos mp{item.first.x, item.first.y, item.first.z};
        visitor(user, &mp, item.second);
    }
    return kWorldOk;
}

std::int32_t getWorldInfoImpl(void* host, ModWorldInfo* out) {
    if (host == nullptr || out == nullptr) return kWorldBadArgs;
    const HostContext* ctx = static_cast<const HostContext*>(host);
    const engine::business::World* world = ctx->world;
    if (world == nullptr) return kWorldNotReady;

    // 两次加锁，两个数字不保证同一时刻的快照——诊断用途，可接受。
    out->loadedChunks = static_cast<std::uint32_t>(world->chunkCount());
    out->totalBlocks  = static_cast<std::uint64_t>(world->blockCount());
    return kWorldOk;
}

} // namespace engine::modhost
