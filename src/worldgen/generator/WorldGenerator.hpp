#pragma once

#include "worldgen/preset/WorldPreset.hpp"

#include <cstddef>

namespace engine::business {
class World;   // 前向声明：生成器只经 World 的生成入口写入，不展开其实现
}

namespace engine::worldgen {

// 世界生成器（#36）：预设（seed + 高度图噪声 + 高度参数 + 地层表）→ 世界方块。
//
// 管线（1a）：seed → 噪声采样每列地表高度 → 按"相对地表深度"查地层表 → 收集方块
//            → World::generateBlocks 批量写入（不走 base 裁决、不发逐块事件）。
//
// 契约：纯函数式——同 preset 必得同世界（sample(x,z,seed) 是纯函数，无隐藏状态）；
//       1a 一次性生成固定区域（x,z ∈ [-24,23]，y ∈ [0,63]）；流式/异步属 1b。
class WorldGenerator {
public:
    // 生成统计（诊断/验收用）
    struct Stats {
        std::size_t columns = 0;    // 生成列数
        std::size_t blocks = 0;     // 写入方块数
        int minSurfaceY = 0;        // 地表高度范围
        int maxSurfaceY = 0;
    };

    // world：目标世界（生成期持有引用并批量写入）；preset：已加载预设（噪声已实例化）
    WorldGenerator(engine::business::World& world, const WorldPreset& preset);

    // 全量生成一次；失败返回 false（stderr 已说明），且不写入任何方块。
    bool generate(Stats* stats = nullptr);

private:
    engine::business::World& world_;
    const WorldPreset& preset_;
};

} // namespace engine::worldgen
