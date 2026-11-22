#pragma once
#include "shared/chunk/ChunkData.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace engine::business::base {

// 基础动作（功能函数层）：对世界数据的原子操作 + 副作用。
// 无状态、无锁 —— 操作谁的数据由调用方传 chunks 引用。
// （B115：原文写"无状态、无锁、纯函数"，"纯函数"是错的：这些函数会写 chunks 与 dirty。）
// 线程安全由调用方负责（业务线程经 World 加锁；其他上下文自行约定）。

// 脏原因常量（ChunkData::dirty() 位掩码；替换 magic number）。
// B114：0x02 / 0x04 两个位是空的 —— 不是漏定义，是留给后续原因的保留位
//       （如"液体流动""爆炸破坏"）。位值可能进存档，所以不重排、不复用。
// B119：这几个常量语义上属于 ChunkData（dirty 是它的字段），暂留此处是为不动
//       调用面；将来归档时应随 dirty 一起搬过去。
inline constexpr std::uint32_t DIRTY_GENERATED = 0x01; // 生成装载标记脏
inline constexpr std::uint32_t DIRTY_PLACED    = 0x08; // 放置标记脏（持久留档）
inline constexpr std::uint32_t DIRTY_REMOVED   = 0x10; // 破坏标记脏（持久留档）

// 标脏：把 cp 自身 + 边界方向邻居（方块 p 若在 chunk 边缘）插入 dirty 集合。
// 集合自动去重；规则只存在这一处（单一事实来源）。
// B116（已评估，本轮不改）：只标 6 个**面**邻居，不标 12 条棱 + 8 个角。
//   当前渲染只有面剔除，棱/角邻居不需要重建；等 AO / 平滑光照落地时必须改成
//   26 邻域，否则边缘会漏更新。现在提前标，每次放置会多重建 5~20 个 chunk，
//   属于为不存在的问题付代价（规则6：性能不提前优化）。
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
