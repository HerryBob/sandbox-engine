#pragma once

#include "shared/block/Block.hpp"

#include <array>
#include <cstddef>
#include <functional> // std::hash 特化（ColPos 作 unordered_set key 用）
#include <memory>

namespace engine::worldgen {

// ---- 生成侧共享类型（1b）----
// 生成粒度（1b 决策点①）：按"2D 列"生成。一列 = 一个 chunk 的 x,z 范围（16×16 方块）
// × 高度 y ∈ [kGenMinY, kGenMaxY]，一次产出 4 个 16³ ChunkData。
// 理由：高度图天然是 2D 的，一次采样 256 个方块列不浪费；若按 3D chunk 逐层请求，
//       地表以上的层（y=48..63 常见）全是空气，一半请求纯空转。

// 一列的水平坐标（chunk 坐标）：覆盖世界方块 x/z ∈ [x*16, x*16+15]。
struct ColPos {
    int x = 0;
    int z = 0;

    bool operator==(const ColPos& rhs) const { return x == rhs.x && z == rhs.z; }
};

// 生成高度范围（沿用 1a；1b 只改水平范围，不改高度）。
constexpr int kGenMinY = 0;
constexpr int kGenMaxY = 63;

// 一列覆盖的竖直 chunk 层数：y ∈ [0,63] → 4 个 16³ chunk。
constexpr int kGenChunkLevels =
    (kGenMaxY - kGenMinY + 1) / engine::shared::CHUNK_SIZE;

static_assert((kGenMaxY - kGenMinY + 1) % engine::shared::CHUNK_SIZE == 0,
              "生成高度范围必须是 16 的整数倍（列产物按 16³ chunk 对齐，否则层数会截断）");

// 一列的生成产物（纯数据，不依赖 World；可被后台线程自由创建/传递）：
//   chunks[i]    = 该列第 i 层的 16³ 方块数据（i = 世界 y / 16）
//   minSurfaceY / maxSurfaceY = 该列 256 个方块列的地表高度范围
//   blockCount   = 该列写入的非空气方块数
struct ColumnBlocks {
    ColPos pos;
    std::array<engine::shared::ChunkData, kGenChunkLevels> chunks{};
    int minSurfaceY = 0;
    int maxSurfaceY = 0;
    std::size_t blockCount = 0;
};

// ---- 后台生成的消息类型（1b）----
// 请求：业务线程 → 生成线程。只带列坐标（预设/seed 由生成器自己持有，不重复传递）。
struct GenRequest {
    ColPos pos;
};

// 结果：生成线程 → 业务线程。
// 载荷用 shared_ptr：SPSCQueue 的 push/pop 是值拷贝，若直接放 ColumnBlocks
// 每次进出队列都要拷 16KB；传指针后拷贝成本 = 一次引用计数，载荷本身只构造一次。
struct GenResult {
    ColPos pos;
    std::shared_ptr<ColumnBlocks> blocks;
};

// 统计快照（诊断/验收口径）：
//   requested = 已入队的请求数
//   generated = 后台已算完的列数
//   applied   = 已写入 World 的列数（正常收尾时三者应相等）
//   blocks / minSurfaceY / maxSurfaceY = 已应用列的聚合（业务侧累加，非后台线程）
struct GenStats {
    std::size_t requested = 0;
    std::size_t generated = 0;
    std::size_t applied = 0;
    std::size_t blocks = 0;
    int minSurfaceY = 0;
    int maxSurfaceY = 0;
};

} // namespace engine::worldgen

// std::hash 特化：让 ColPos 可作 unordered_set 的 key（已请求列的记账）
namespace std {
template <>
struct hash<engine::worldgen::ColPos> {
    size_t operator()(const engine::worldgen::ColPos& p) const noexcept {
        // 与 BlockPos 同款混合散列（2D 列坐标）
        size_t h = static_cast<size_t>(p.x);
        h = h * 31 + static_cast<size_t>(p.z);
        return h;
    }
};
} // namespace std
