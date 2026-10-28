#pragma once
#include "shared/block/Block.hpp"

#include <functional> // std::function（forEachBlockInAABB 回调）
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::business {

// 世界数据的所有者（source of truth，事实来源）：方块存储 + 锁 + 查询 + 动作入口。
// 全引擎只有 World 能持有方块数据；所有修改必须经过这里的锁（锁跟着数据走）。
// 本类只描述"世界是什么/怎么改"，不决定"何时改"（调度是 BusinessWorld 的职责）。
//
// 职责边界：
//   状态层：chunks_（方块存储）+ blocksMutex_（锁）
//   查询：findBlock / renderSnapshot / forEachBlockInAABB ...
//   动作入口：placeBlock / removeBlock（加锁 → base::tryPlace/tryRemove，规则在 base）
// 不含：调度逻辑（BusinessWorld）、桥/协议（PhysicsBridge/RenderBridge）、
//       规则实现（base/BlockActions）、锁上下文外的任何裸数据访问。
class World {
public:
    // 初始世界：48×48 石头平台（x,z ∈ [-24,23]，y=0）。
    // 初始内容属世界域；未来被世界生成器（chunk 加载/卸载）取代。
    World();

    // ---- 查询（业务/渲染线程只读通道；锁内访问，安全）----
    // 方块总数（main 线程只读 + 业务线程结构性修改并发——加锁读）。
    std::size_t blockCount() const;
    // 渲染快照：锁内拷贝全量方块列表（供渲染线程安全调用）。
    std::vector<engine::shared::Block> renderSnapshot() const;
    // 按坐标查主世界方块；返回空 = 不存在。chunk 定位 O(1)。
    std::optional<engine::shared::Block> findBlock(const engine::shared::BlockPos& p) const;
    // 已加载 chunk 数（空 chunk 不占内存；断言/诊断用）。
    std::size_t chunkCount() const;
    // 区域查询（跨 chunk；同步/LOD/变更广播共用）：遍历 [min, max] 内所有非空气方块。
    void forEachBlockInAABB(const engine::shared::BlockPos& min,
                            const engine::shared::BlockPos& max,
                            const std::function<void(const engine::shared::BlockPos&,
                                                     engine::shared::BlockType)>& fn) const;
    // 某 chunk 的变更原因位掩码（修改跟踪诊断用；未加载返回 0）。
    std::uint32_t chunkDirtyFlags(const engine::shared::ChunkPos& cp) const;

    // ---- 动作入口（所有修改世界的唯一合法路径，内部加锁）----
    // 裁决在 base（占用/不存在 → 拒绝）；返回 false = 拒绝（计数由调用方管）。
    bool placeBlock(const engine::shared::BlockPos& p, engine::shared::BlockType type);
    bool removeBlock(const engine::shared::BlockPos& p);

    // 本 tick 变脏的 chunk 集合：拷贝查看（不消费）。
    // 调用方（BusinessWorld::flushChanges）push 变更事件成功后再 clearDirtyChunks()；
    // push 失败不清空 → 下 tick 重试（丢变更 = 渲染端漏重建）。
    // dirty 只被业务线程触碰（动作写入 / flushChanges 消费）→ 不加锁。
    std::unordered_set<engine::shared::ChunkPos> peekDirtyChunks() const;
    void clearDirtyChunks();

private:
    // 方块存储：16³ 密集 uint8，空 chunk 不占内存，定位 O(1)。
    // 业务线程结构性修改与 main 只读并发 → blocksMutex_ 保护。
    std::unordered_map<engine::shared::ChunkPos, engine::shared::ChunkData> chunks_;
    mutable std::mutex blocksMutex_; // const 读函数也要能加锁（blockCount/renderSnapshot）

    // 变更事件流的事件源：本 tick 变脏的 chunk 集合（写操作插入，自动去重）。
    // flushChanges 在 tick 边界采集打包为 BlockChangeEvent 后清空（业务线程专用）。
    std::unordered_set<engine::shared::ChunkPos> dirtyChunksThisTick_;
};

} // namespace engine::business
