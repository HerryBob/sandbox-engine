#include "business/World.hpp"
#include "business/base/BlockActions.hpp"

#include <cstdint>

namespace engine::business {

World::World() {
    // 初始世界：48×48 石头平台（x,z ∈ [-24,23]，y=0）。
    // 数据层 ② 区块化：方块写入 16³ chunk（chunks_ 稀疏映射，空 chunk 不占内存）。
    // 初始内容属世界域；未来被世界生成器（chunk 加载/卸载）取代。
    auto setBlock = [&](int x, int y, int z, engine::shared::BlockType t) {
        const engine::shared::BlockPos p{x, y, z};
        auto& c = chunks_[engine::shared::chunkOf(p)]; // operator[] 缺省构造新 chunk
        c.atWorld(p) = static_cast<std::uint8_t>(t);
        c.markDirty(0x01); // 修改跟踪：初始装载标记脏（变更事件流种子）
    };
    for (int x = -24; x <= 23; ++x)
        for (int z = -24; z <= 23; ++z)
            setBlock(x, 0, z, engine::shared::BlockType::Stone);
    // 测试块：玻璃（Cutout 分桶验证——悬浮平台上方一格，6 面全暴露）
    setBlock(0, 1, 0, engine::shared::BlockType::Glass);
}

std::size_t World::blockCount() const {
    // main 线程只读（日志/断言）与业务线程结构性修改可能并发——加锁读。
    // 区块化后 = chunk 非空气方块数。
    std::lock_guard<std::mutex> lock(blocksMutex_);
    std::size_t n = 0;
    for (const auto& [cp, data] : chunks_) {
        for (const std::uint8_t t : data.blocks)
            if (t != static_cast<std::uint8_t>(engine::shared::BlockType::Air)) ++n;
    }
    return n;
}

std::vector<engine::shared::Block> World::renderSnapshot() const {
    // 第 4 轨只读：与业务线程结构性修改并发，锁内生成一份安全快照。
    // 数据层 ② 区块化：遍历所有已加载 chunk 的非空气方块。
    std::lock_guard<std::mutex> lock(blocksMutex_);
    std::vector<engine::shared::Block> out;
    out.reserve(chunks_.size() * 64); // 粗略预留，减少扩容
    for (const auto& [cp, data] : chunks_) {
        const int bx = cp.x << 4, by = cp.y << 4, bz = cp.z << 4; // chunk 起点世界坐标
        for (int i = 0; i < engine::shared::CHUNK_VOLUME; ++i) {
            const std::uint8_t t = data.blocks[i];
            if (t == static_cast<std::uint8_t>(engine::shared::BlockType::Air)) continue;
            out.push_back(engine::shared::Block{
                static_cast<engine::shared::BlockType>(t),
                engine::shared::BlockPos{bx + (i & 15), by + ((i >> 4) & 15), bz + (i >> 8)},
                -1}); // 主世界方块 contraptionId 恒 -1（chunk 不存该字段）
        }
    }
    return out;
}

std::optional<engine::shared::Block> World::findBlock(const engine::shared::BlockPos& p) const {
    // 调用方需保证无并发写（业务线程持锁调用 / main 在三轨停止后调用）。
    // 主世界 → chunk 定位 O(1)（chunk 存储只存类型，返回现场构造的 Block）。
    const auto it = chunks_.find(engine::shared::chunkOf(p));
    if (it == chunks_.end()) return std::nullopt;
    const std::uint8_t t = it->second.atWorld(p);
    if (t == static_cast<std::uint8_t>(engine::shared::BlockType::Air))
        return std::nullopt;
    return engine::shared::Block{static_cast<engine::shared::BlockType>(t), p, -1};
}

std::size_t World::chunkCount() const {
    std::lock_guard<std::mutex> lock(blocksMutex_);
    return chunks_.size();
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
                const std::uint8_t t = it->second.atWorld(p);
                if (t == static_cast<std::uint8_t>(engine::shared::BlockType::Air)) continue;
                fn(p, static_cast<engine::shared::BlockType>(t));
            }
}

std::uint32_t World::chunkDirtyFlags(const engine::shared::ChunkPos& cp) const {
    std::lock_guard<std::mutex> lock(blocksMutex_);
    const auto it = chunks_.find(cp);
    return it == chunks_.end() ? 0u : it->second.dirty;
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

std::unordered_set<engine::shared::ChunkPos> World::peekDirtyChunks() const {
    // dirty 只被业务线程触碰（动作写入 / flushChanges 消费）→ 不加锁。
    // 拷贝返回（不消费）：push 成功后才 clear，失败保留下 tick 重试。
    return dirtyChunksThisTick_;
}

void World::clearDirtyChunks() {
    dirtyChunksThisTick_.clear();
}

} // namespace engine::business
