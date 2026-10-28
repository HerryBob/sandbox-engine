#pragma once
#include "shared/block/Block.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace engine::business::base {

// 基础动作（功能函数层）：对世界数据的原子操作 + 副作用。
// 无状态、无锁、纯函数——操作谁的数据由调用方传 chunks 引用。
// 线程安全由调用方负责（业务线程经 World 加锁；其他上下文自行约定）。

// 脏原因常量（ChunkData.dirty 位掩码；替换 magic number）。
inline constexpr std::uint32_t DIRTY_PLACED  = 0x08; // 放置标记脏（持久留档）
inline constexpr std::uint32_t DIRTY_REMOVED = 0x10; // 破坏标记脏（持久留档）

// 标脏：把 cp 自身 + 边界方向邻居（方块 p 若在 chunk 边缘）插入 dirty 集合。
// 集合自动去重；规则只存在这一处（单一事实来源）。
void markDirtyWithBoundary(std::unordered_set<engine::shared::ChunkPos>& dirty,
                           const engine::shared::ChunkPos& cp,
                           const engine::shared::BlockPos& p);

// 放置：裁决（位置已占用 → 拒绝）→ 写入 → 标脏。成功返回 true。
bool tryPlace(std::unordered_map<engine::shared::ChunkPos, engine::shared::ChunkData>& chunks,
              std::unordered_set<engine::shared::ChunkPos>& dirty,
              const engine::shared::BlockPos& p,
              engine::shared::BlockType type);

// 破坏：裁决（方块不存在 → 拒绝）→ 写空气 → 标脏。成功返回 true。
bool tryRemove(std::unordered_map<engine::shared::ChunkPos, engine::shared::ChunkData>& chunks,
               std::unordered_set<engine::shared::ChunkPos>& dirty,
               const engine::shared::BlockPos& p);

} // namespace engine::business::base
