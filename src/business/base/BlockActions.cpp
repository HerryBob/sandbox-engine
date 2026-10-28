#include "business/base/BlockActions.hpp"

namespace engine::business::base {

void markDirtyWithBoundary(std::unordered_set<engine::shared::ChunkPos>& dirty,
                           const engine::shared::ChunkPos& cp,
                           const engine::shared::BlockPos& p) {
    dirty.insert(cp); // 变更事件流：本 tick 事件源
    // 判断是否边界写入：块内局部坐标 &15 落在 0/15 → 该方向邻居也要重建
    const int lx = p.x & 15, ly = p.y & 15, lz = p.z & 15;
    if (lx == 0)  dirty.insert({ cp.x - 1, cp.y, cp.z });
    if (lx == 15) dirty.insert({ cp.x + 1, cp.y, cp.z });
    if (ly == 0)  dirty.insert({ cp.x, cp.y - 1, cp.z });
    if (ly == 15) dirty.insert({ cp.x, cp.y + 1, cp.z });
    if (lz == 0)  dirty.insert({ cp.x, cp.y, cp.z - 1 });
    if (lz == 15) dirty.insert({ cp.x, cp.y, cp.z + 1 });
}

bool tryPlace(std::unordered_map<engine::shared::ChunkPos, engine::shared::ChunkData>& chunks,
              std::unordered_set<engine::shared::ChunkPos>& dirty,
              const engine::shared::BlockPos& p,
              engine::shared::BlockType type) {
    // 裁决：位置已被占用（chunk 存在且该格非空气）→ 拒绝放置。
    const auto cp = engine::shared::chunkOf(p);
    const auto it = chunks.find(cp);
    if (it != chunks.end() &&
        it->second.atWorld(p) != static_cast<std::uint8_t>(engine::shared::BlockType::Air))
        return false;
    // 区块化：chunk 写入（16³ 密集数组，O(1)；不存在则默认构造空 chunk）。
    auto& c = chunks[cp];
    c.atWorld(p) = static_cast<std::uint8_t>(type);
    c.markDirty(DIRTY_PLACED); // 修改跟踪：放置标记脏（持久留档）
    markDirtyWithBoundary(dirty, cp, p);
    return true;
}

bool tryRemove(std::unordered_map<engine::shared::ChunkPos, engine::shared::ChunkData>& chunks,
               std::unordered_set<engine::shared::ChunkPos>& dirty,
               const engine::shared::BlockPos& p) {
    // 裁决：目标方块必须存在（chunk 存在且该格非空气）。
    const auto it = chunks.find(engine::shared::chunkOf(p));
    if (it == chunks.end() ||
        it->second.atWorld(p) == static_cast<std::uint8_t>(engine::shared::BlockType::Air))
        return false;
    it->second.atWorld(p) =
        static_cast<std::uint8_t>(engine::shared::BlockType::Air);
    it->second.markDirty(DIRTY_REMOVED); // 修改跟踪：破坏标记脏（持久留档）
    markDirtyWithBoundary(dirty, it->first, p);
    return true;
}

} // namespace engine::business::base
