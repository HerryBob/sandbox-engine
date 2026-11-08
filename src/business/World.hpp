#pragma once
#include "shared/block/Block.hpp"

#include <cstdint>
#include <functional> // std::function（forEachBlockInAABB 回调）
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::business {

// 世界方块数据的持有者：存储、锁、只读查询、修改入口。
// 所有方块数据的读写都必须经过本类的锁；修改时机由 BusinessWorld 决定。
class World {
public:
    // 构造初始世界：48×48 石头平台（x,z ∈ [-24,23]，y=0）。
    World();

    // ---- 只读查询（内部加锁，任意线程可调）----
    // 已加载 chunk 内的非空气方块总数。
    std::size_t blockCount() const;
    // 拷贝全部非空气方块（加锁）。用于首帧与世界整体重建。
    std::vector<engine::shared::Block> renderSnapshot() const;
    // 拷贝指定 chunk 及其邻居内的非空气方块（加锁）。
    // chunks：目标 chunk 集合；expandNeighbors：是否连带 6 邻居（面剔除需要块外数据）。
    // 未加载的 chunk 跳过。
    std::vector<engine::shared::Block> chunkSnapshot(
        const std::unordered_set<engine::shared::ChunkPos>& chunks,
        bool expandNeighbors = true) const;
    // 按世界坐标查方块；返回空 = 该格不存在（未加载或空气）。
    std::optional<engine::shared::Block> findBlock(const engine::shared::BlockPos& p) const;

    // 单格查询的三种结果。
    enum class BlockState : std::uint8_t {
        NotLoaded = 0,  // 所在 chunk 未加载
        Air       = 1,  // 已加载，该格是空气
        Occupied  = 2,  // 已加载，该格有方块
    };
    // p：世界坐标；outType：仅返回 Occupied 时写入。
    BlockState lookupBlock(const engine::shared::BlockPos& p,
                           engine::shared::BlockType& outType) const;
    // 已加载的非空 chunk 数量。
    std::size_t chunkCount() const;

    // 调色板统计（验收用）：索引位宽分布、字典项数、实际占用字节。
    struct PaletteStats {
        std::size_t chunks = 0;
        std::size_t bits4 = 0;
        std::size_t bits8 = 0;
        std::size_t bits16 = 0;
        std::size_t paletteEntries = 0;
        std::size_t totalBytes = 0;
        double avgPalette = 0.0;
    };
    PaletteStats paletteStats() const;
    // 遍历 [min, max] 闭区间内的非空气方块；回调在锁内执行，不可重入。
    void forEachBlockInAABB(const engine::shared::BlockPos& min,
                            const engine::shared::BlockPos& max,
                            const std::function<void(const engine::shared::BlockPos&,
                                                     engine::shared::BlockType)>& fn) const;

    // ---- 修改入口（内部加锁）----
    // 放置方块；规则校验在 base::tryPlace，返回 false = 被拒绝。
    bool placeBlock(const engine::shared::BlockPos& p, engine::shared::BlockType type);
    // 移除方块；规则校验在 base::tryRemove，返回 false = 被拒绝。
    bool removeBlock(const engine::shared::BlockPos& p);

    // ---- 生成写入（流式加载的地形落地）----
    // 覆盖一个 16³ chunk，并标脏自身与已存在的 6 邻居；全空气 chunk 不创建。
    // 只允许业务线程调用。
    void applyGeneratedChunk(const engine::shared::ChunkPos& cp,
                             const engine::shared::ChunkData& data);

    // ---- 区块卸载 ----
    // 删除指定 chunk 并标脏自身与 6 邻居；返回实际删除数量（未加载的忽略）。
    // 只允许业务线程调用。
    std::size_t unloadChunks(const std::unordered_set<engine::shared::ChunkPos>& cps);

    // 本 tick 变脏的 chunk 集合，只读不消费；清空由 clearDirtyChunks 负责。
    // 仅业务线程访问，不加锁。
    std::unordered_set<engine::shared::ChunkPos> peekDirtyChunks() const;
    // 清空本 tick 的变脏集合（推送成功后调用）。
    void clearDirtyChunks();

private:
    // chunk 数据表：key = chunk 坐标，value = 16³ 方块数据（调色板 + 索引）；空 chunk 不入表。
    std::unordered_map<engine::shared::ChunkPos, engine::shared::ChunkData> chunks_;
    mutable std::mutex blocksMutex_; // 保护 chunks_；const 成员函数也要加锁

    // 本 tick 变脏的 chunk 集合（写操作插入，自动去重）；flushChanges 消费后清空。
    std::unordered_set<engine::shared::ChunkPos> dirtyChunksThisTick_;
};

} // namespace engine::business
