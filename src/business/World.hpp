#pragma once
#include "shared/chunk/ChunkData.hpp"

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
//
// 线程模型（B81：原先散落在 .cpp 与各函数注释里，集中在此）：
//   · 数据本身（chunks_ + chunksLock_）—— **任意线程**可安全读写（内部加锁）；
//   · 脏集合（dirtyChunksThisTick_）—— **仅业务线程**，不加锁（见 B84 说明）；
//   · applyGeneratedChunk / unloadChunks —— **仅业务线程**（唯一 World 写者）。
class World {
public:
    // B80：显式禁止拷贝 —— 内含 mutex 与两个大表，拷贝既无意义也不安全
    // （原来只是隐式删除，读者要自己推）。
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    // 构造空世界：不预置任何方块。方块内容全部来自模组注册 + 流式生成的写入
    // （B79：此处原注释写"48×48 石头平台"，那是引擎内置方块时代的行为，早已删除，
    //   按旧注释推理会完全错判初始世界为空这件事）。
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
    // B82：与下面的三态 lookupBlock **功能重叠** —— 这个把"未加载"和"空气"都返回空，
    // 需要区分时必须用 lookupBlock。保留两态版是因为大多数调用方只关心"有没有方块"。
    std::optional<engine::shared::Block> findBlock(const engine::shared::BlockPos& p) const;

    // 单格查询的三种结果。
    enum class LookupResult : std::uint8_t {
        NotLoaded = 0,  // 所在 chunk 未加载
        Air       = 1,  // 已加载，该格是空气
        Occupied  = 2,  // 已加载，该格有方块
    };
    // p：世界坐标；outType：仅返回 Occupied 时写入。
    LookupResult lookupBlock(const engine::shared::BlockPos& p,
                           engine::shared::BlockType& outType) const;
    // 已加载的非空 chunk 数量。
    std::size_t chunkCount() const;

    // 调色板统计（验收用）：索引位宽分布、字典项数、实际占用字节。
    // 验收/诊断专用：不属于 World 的核心职责。
    // 统计各 chunk 的调色板位宽分布和内存占用，用于验证调色板压缩效果。
    // 重构时应移到独立的诊断模块（如 PaletteDiagnostics），World 只提供
    // "遍历所有 chunk" 的能力，统计逻辑由诊断模块自己做。
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
    // B85：**前置条件缺失** —— 直接删，不检查该 chunk 是否已存盘。当前没有存档系统，
    // 所以无害；接存档后必须在调用方保证"已写盘"，或在这里加检查（否则会丢数据）。
    // B128：这里标脏，渲染端重建时取不到方块 → 删掉该 chunk 的 mesh。
    //       这是一条**三环隐式契约**：unloadChunks 标脏 → RenderBridge 汇总事件 →
    //       VulkanContext::rebuildChunkMeshes 的"无顶点则删 mesh"分支。
    //       任何一环被"优化"掉（比如渲染端把无顶点改成 continue），就会留下幽灵网格。
    //       已在该分支写死注释，改动前请连同这里一起看。
    std::size_t unloadChunks(const std::unordered_set<engine::shared::ChunkPos>& cps);

    // 本 tick 变脏的 chunk 集合，只读不消费；清空由 clearDirtyChunks 负责。
    // 仅业务线程访问，不加锁。
    // B84：注意**锁不对称** —— 写入（tryPlace/tryRemove 的 dirty 参数）发生在
    // chunksLock_ 内，但读取（peek）与清空（clear）不加锁。这不是漏写锁，
    // 而是脏集合归业务线程独占；不对称容易让读者误以为它受锁保护，故在此写明。
    std::unordered_set<engine::shared::ChunkPos> peekDirtyChunks() const;
    // 清空本 tick 的变脏集合（推送成功后调用）。
    void clearDirtyChunks();

private:
    // chunk 数据表：key = chunk 坐标，value = 16³ 方块数据（调色板 + 索引）；空 chunk 不入表。
    std::unordered_map<engine::shared::ChunkPos, engine::shared::ChunkData> chunks_;
    mutable std::mutex chunksLock_; // 保护 chunks_；const 成员函数也要加锁

    // 本 tick 变脏的 chunk 集合（写操作插入，自动去重）；flushChanges 消费后清空。
    std::unordered_set<engine::shared::ChunkPos> dirtyChunksThisTick_;
};

} // namespace engine::business
