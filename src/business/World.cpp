#include "business/World.hpp"
#include "business/base/BlockActions.hpp"

#include <cstdint>

namespace engine::business {

namespace {
// 把一个 chunk 的非空气方块按世界坐标追加到 out。
// renderSnapshot（全量快照）与 chunkSnapshot（增量快照）共用这一份实现：
// 索引 → 块内偏移的解码只在 ChunkData::localOf 定义，两处快照不再各写一遍位运算
// （那种重复改一处忘一处就是体素引擎里极难查的坐标错位）。
void appendChunkBlocks(const engine::shared::ChunkPos& cp,
                       const engine::shared::ChunkData& data,
                       std::vector<engine::shared::Block>& out) {
    const int bx = cp.x << 4, by = cp.y << 4, bz = cp.z << 4; // chunk 起点世界坐标
    for (std::int32_t i = 0; i < engine::shared::CHUNK_VOLUME; ++i) {
        const engine::shared::PaletteEntry t = data.getAt(i);
        if (t == engine::shared::kAirEntry) continue;
        const engine::shared::BlockPos off = engine::shared::ChunkData::localOf(i);
        out.push_back(engine::shared::Block{
            static_cast<engine::shared::BlockType>(t),
            engine::shared::BlockPos{bx + off.x, by + off.y, bz + off.z},
            -1}); // 主世界方块 contraptionId 恒 -1（chunk 不存该字段）
    }
}

// 六邻接偏移：chunk 到达/消失都会改变相邻 chunk 边界面的剔除结果。
// 标脏与快照扩邻居共用这一份。
constexpr int kNeighborOffsets[6][3] = {
    { 1, 0, 0}, {-1, 0, 0},
    { 0, 1, 0}, { 0,-1, 0},
    { 0, 0, 1}, { 0, 0,-1}};
} // namespace

World::World() {
    // 初始世界由装配层（main）用 ColumnGenerator + ChunkStreamer 从数据预设生成。
    // 构造函数不再预置方块：世界域的初始内容 = data/worldgen/presets/*.json。
}

std::size_t World::blockCount() const {
    // main 线程只读（日志/断言）与业务线程结构性修改可能并发——加锁读。
    // 区块化后 = chunk 非空气方块数。
    std::lock_guard<std::mutex> lock(blocksMutex_);
    std::size_t n = 0;
    for (const auto& kv : chunks_) n += kv.second.nonAirCount();
    return n;
}

std::vector<engine::shared::Block> World::renderSnapshot() const {
    // 第 4 轨只读：与业务线程结构性修改并发，锁内生成一份安全快照。
    // 数据层 ② 区块化：遍历所有已加载 chunk 的非空气方块。
    std::lock_guard<std::mutex> lock(blocksMutex_);
    std::vector<engine::shared::Block> out;
    out.reserve(chunks_.size() * 64); // 粗略预留，减少扩容
    for (const auto& [cp, data] : chunks_) {
        appendChunkBlocks(cp, data, out);
    }
    return out;
}

std::vector<engine::shared::Block> World::chunkSnapshot(
    const std::unordered_set<engine::shared::ChunkPos>& chunks,
    bool expandNeighbors) const {
    // 增量重建取数：只导"目标 chunk ∪ 6 邻居"，其余 chunk 一律不碰。
    //
    // 为什么无条件扩邻居（不看被改方块位置）：
    //   重建的单位是整块 chunk（4096 格），其中边界壳有 1352 格（33%）至少一个邻居在块外。
    //   面剔除 computeFaceMask 要查它们的 6 个直接邻居，不带邻居 → 这些面全被判"暴露"
    //   → 整块边缘长出穿透面。标脏那边看"方块在不在边界"是对的（只多标该方向的邻居），
    //   但取数面向整块，六个方向必须一次带满。
    // 复杂度：O(目标 chunk 数 × 4096)，与全世界无关。未加载邻居跳过 → 面保持暴露，
    //   与 renderSnapshot 全量快照的行为一致（窗口边缘本来就没有数据）。
    // 先把"要读的 chunk"算成集合：dirty ∪ 各自 6 邻居（重复自动去重）
    std::unordered_set<engine::shared::ChunkPos> targets;
    targets.reserve(chunks.size() * 7);
    for (const auto& cp : chunks) {
        targets.insert(cp);
        if (!expandNeighbors) continue;
        for (const auto& off : kNeighborOffsets) {
            targets.insert(engine::shared::ChunkPos{
                cp.x + off[0], cp.y + off[1], cp.z + off[2]});
        }
    }

    std::lock_guard<std::mutex> lock(blocksMutex_);
    std::vector<engine::shared::Block> out;
    out.reserve(targets.size() * 512); // 实测约 3156 非空气/chunk，512 起步留扩容余量
    for (const auto& cp : targets) {
        const auto it = chunks_.find(cp);
        if (it == chunks_.end()) continue; // 未加载：跳过（面保持暴露）
        appendChunkBlocks(cp, it->second, out);
    }
    return out;
}

std::optional<engine::shared::Block> World::findBlock(const engine::shared::BlockPos& p) const {
    // 与其它读函数一致：锁内访问 chunks_（全引擎唯一数据真源，读不加锁会与写路径竞争）。
    // 调用频率：射线检测 25 TPS × 约 25 步 ≈ 625 次/秒，加锁成本可忽略。
    // 约束：blocksMutex_ 是不可重入的 std::mutex，持有本锁的函数内部不得再调用本函数，
    //       否则自死锁。当前 placeBlock / removeBlock / applyGeneratedChunk /
    //       forEachBlockInAABB 均未在锁内调用它。
    // 主世界 → chunk 定位 O(1)（chunk 存储只存类型，返回现场构造的 Block）。
    std::lock_guard<std::mutex> lock(blocksMutex_);
    const auto it = chunks_.find(engine::shared::chunkOf(p));
    if (it == chunks_.end()) return std::nullopt;
    const engine::shared::PaletteEntry t = it->second.get(p);
    if (t == engine::shared::kAirEntry) return std::nullopt;
    return engine::shared::Block{static_cast<engine::shared::BlockType>(t), p, -1};
}

World::BlockState World::lookupBlock(const engine::shared::BlockPos& p,
                                     engine::shared::BlockType& outType) const {
    // 三态判定与取值必须在同一次锁内完成：分两次调用可能夹着一次 chunk 卸载，
    // 给出互相矛盾的结论。
    std::lock_guard<std::mutex> lock(blocksMutex_);
    const auto it = chunks_.find(engine::shared::chunkOf(p));
    if (it == chunks_.end()) return BlockState::NotLoaded;
    const engine::shared::PaletteEntry t = it->second.get(p);
    if (t == engine::shared::kAirEntry) return BlockState::Air;
    outType = static_cast<engine::shared::BlockType>(t);
    return BlockState::Occupied;
}

std::size_t World::chunkCount() const {
    std::lock_guard<std::mutex> lock(blocksMutex_);
    return chunks_.size();
}

World::PaletteStats World::paletteStats() const {
    std::lock_guard<std::mutex> lock(blocksMutex_);
    PaletteStats s;
    s.chunks = chunks_.size();
    for (const auto& kv : chunks_) {
        const engine::shared::ChunkData& d = kv.second;
        if (d.bits() == 4)      ++s.bits4;
        else if (d.bits() == 8) ++s.bits8;
        else                    ++s.bits16;
        s.paletteEntries += d.paletteSize();
        s.totalBytes     += d.byteSize();
    }
    if (s.chunks != 0)
        s.avgPalette = static_cast<double>(s.paletteEntries) / static_cast<double>(s.chunks);
    return s;
}

void World::forEachBlockInAABB(
    const engine::shared::BlockPos& min, const engine::shared::BlockPos& max,
    const std::function<void(const engine::shared::BlockPos&,
                             engine::shared::BlockType)>& fn) const {
    // 区域查询（跨 chunk；同步/LOD 生成/变更广播共用）：遍历区域内每个坐标，
    // chunk 定位 O(1)。原型按坐标遍历（区域通常 = chunk 级 16³=4096 次查询，可接受；
    // 渲染端想烘焙 mesh 时扩展 1 格拉邻居即可，Minetest retrieveBlocks 同思想）。
    std::lock_guard<std::mutex> lock(blocksMutex_);
    for (int x = min.x; x <= max.x; ++x)
        for (int y = min.y; y <= max.y; ++y)
            for (int z = min.z; z <= max.z; ++z) {
                const engine::shared::BlockPos p{x, y, z};
                const auto it = chunks_.find(engine::shared::chunkOf(p));
                if (it == chunks_.end()) continue;
                const engine::shared::PaletteEntry t = it->second.get(p);
                if (t == engine::shared::kAirEntry) continue;
                fn(p, static_cast<engine::shared::BlockType>(t));
            }
}

bool World::placeBlock(const engine::shared::BlockPos& p,
                       engine::shared::BlockType type) {
    std::lock_guard<std::mutex> lock(blocksMutex_);
    // 裁决 + 写入 + 标脏在 base（占用拒绝）；锁在此处（数据修改的唯一合法入口）。
    return base::tryPlace(chunks_, dirtyChunksThisTick_, p, type);
}

bool World::removeBlock(const engine::shared::BlockPos& p) {
    std::lock_guard<std::mutex> lock(blocksMutex_);
    // 裁决 + 写入 + 标脏在 base（不存在拒绝）；锁在此处。
    return base::tryRemove(chunks_, dirtyChunksThisTick_, p);
}

void World::applyGeneratedChunk(const engine::shared::ChunkPos& cp,
                                const engine::shared::ChunkData& data) {
    // 运行期生成写入（1b）：一次锁 → 整体覆盖 → 标脏自身与邻居。
    std::lock_guard<std::mutex> lock(blocksMutex_);

    // 全空气 chunk 不创建（保持"空 chunk 不占内存"；地表以上常见）
    if (data.isAllAir() && chunks_.find(cp) == chunks_.end()) return;

    auto& slot = chunks_[cp];   // 已有则取用、没有则缺省构造
    slot = data;                // 整体覆盖：生成是权威写入，不做旧值合并
    slot.markDirty(0x01);       // 修改跟踪：生成装载

    // 运行期必须进脏集合：渲染端只按它请求增量重建
    dirtyChunksThisTick_.insert(cp);
    // 邻居一并标脏：新 chunk 会改变相邻 chunk 边界面的剔除结果
    for (const auto& off : kNeighborOffsets) {
        const engine::shared::ChunkPos n{
            cp.x + off[0], cp.y + off[1], cp.z + off[2]};
        if (chunks_.find(n) != chunks_.end()) dirtyChunksThisTick_.insert(n);
    }
}

std::size_t World::unloadChunks(const std::unordered_set<engine::shared::ChunkPos>& cps) {
    // 区块卸载（流式加载的回收端）：删除数据 + 标脏"自身与还在的邻居"。
    // 渲染端不需要新接口：重建一个已不存在的 chunk 时取不到方块 → 无顶点 →
    // VulkanContext 自动 destroyChunkMesh 并 erase，内存随之释放。
    std::lock_guard<std::mutex> lock(blocksMutex_);
    std::size_t removed = 0;
    for (const auto& cp : cps) {
        if (chunks_.erase(cp) == 0) continue;   // 本来就没加载：不标脏、不计数
        ++removed;
        dirtyChunksThisTick_.insert(cp);        // 自身：让渲染端把旧 mesh 删掉
        for (const auto& off : kNeighborOffsets) {
            const engine::shared::ChunkPos n{
                cp.x + off[0], cp.y + off[1], cp.z + off[2]};
            // 邻居面剔除依赖被删的 chunk：边界面从"被遮挡"变"暴露"，必须重建
            if (chunks_.find(n) != chunks_.end()) dirtyChunksThisTick_.insert(n);
        }
    }
    return removed;
}

std::unordered_set<engine::shared::ChunkPos> World::peekDirtyChunks() const {
    // dirty 只被业务线程触碰（动作写入 / flushChanges 消费）→ 不加锁。
    // 拷贝返回（不消费）：push 成功后才 clear，失败保留下 tick 重试。
    return dirtyChunksThisTick_;
}

void World::clearDirtyChunks() {
    dirtyChunksThisTick_.clear();
}

} // namespace engine::business
