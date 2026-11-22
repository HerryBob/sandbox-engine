#include "business/base/BlockActions.hpp"

namespace engine::business::base {

void markDirtyWithBoundary(std::unordered_set<engine::shared::ChunkPos>& dirty,
                           const engine::shared::ChunkPos& cp,
                           const engine::shared::BlockPos& p) {
    dirty.insert(cp); // 变更事件流：本 tick 事件源
    // B121：邻居 chunk 可能根本没加载 —— 这里照样插进 dirty 集合（去重集合，代价是一次哈希）。
    // 渲染端重建时找不到该 chunk 会自然跳过，所以不需要在这里查表（查了反而多一次 map 查找）。
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
    // B117：Air 不是"要放的方块"。不拦它会：给一个位置标脏（数据其实没变），
    // 并且因为下面 `chunks[cp]` 会默认构造，凭空建出一个全空气 chunk 占内存。
    if (type == engine::shared::BlockType::Air) return false;

    // 裁决：位置已被占用（chunk 存在且该格非空气）→ 拒绝放置。
    // B120：与 tryRemove 保持同一套局部名（chunkPos / it）。
    const auto chunkPos = engine::shared::toChunkOf(p);
    const auto it = chunks.find(chunkPos);
    if (it != chunks.end() && it->second.paletteGet(p) != engine::shared::kAirEntry)
        return false;
    // 区块化：chunk 写入（调色板 + 索引，O(1)；不存在则默认构造空 chunk）。
    auto& c = chunks[chunkPos];
    c.paletteSet(p, static_cast<engine::shared::PaletteEntry>(type));
    c.markDirty(DIRTY_PLACED); // 修改跟踪：放置标记脏（持久留档）
    markDirtyWithBoundary(dirty, chunkPos, p);
    return true;
}

bool tryRemove(std::unordered_map<engine::shared::ChunkPos, engine::shared::ChunkData>& chunks,
               std::unordered_set<engine::shared::ChunkPos>& dirty,
               const engine::shared::BlockPos& p) {
    // 裁决：目标方块必须存在（chunk 存在且该格非空气）。
    const auto chunkPos = engine::shared::toChunkOf(p);
    const auto it = chunks.find(chunkPos);
    if (it == chunks.end() || it->second.paletteGet(p) == engine::shared::kAirEntry)
        return false;
    it->second.paletteSet(p, engine::shared::kAirEntry);
    it->second.markDirty(DIRTY_REMOVED); // 修改跟踪：破坏标记脏（持久留档）
    markDirtyWithBoundary(dirty, chunkPos, p);
    return true;
}

} // namespace engine::business::base
