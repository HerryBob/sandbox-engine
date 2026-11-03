#pragma once

#include "worldgen/noise/NoiseSource.hpp"

#include <memory>
#include <string>
#include <vector>

namespace engine::worldgen {

// 地层：一个方块在"相对地表深度"区间的填充。
// depthMin/depthMax 为相对地表深度（0 = 地表）——#36 决策点 1 草案，
// 待用户拍板（相对地表 vs 绝对高度区间）；此结构按相对深度语义注释。
struct Stratum {
    std::string block;    // 方块注册名（如 "grass" / "dirt" / "stone"）
    int depthMin = 0;     // 相对地表深度下界（含）
    int depthMax = 0;     // 相对地表深度上界（含）
};

// 世界预设（纯数据装配结果）：一个世界怎么生成的全部参数。
// 来源 = data/worldgen/presets/default.json（#35 PresetLoader 解析）；
// heightmap 是已实例化的噪声对象（NoiseRegistry 查表所得），WorldGenerator 直接采样。
// 注意：含 unique_ptr 成员 → 本结构可移动、不可拷贝（噪声独占所有权，语义正确）。
struct WorldPreset {
    std::uint64_t seed = 0;
    std::unique_ptr<NoiseSource> heightmap;   // 高度图噪声（地表高度来源）
    double baseHeight = 32.0;                 // 高度映射：基准高度（起伏中点）
    double amplitude = 16.0;                  // 高度映射：起伏幅度（±）
    std::vector<Stratum> strata;              // 地层表（按相对地表深度）
};

} // namespace engine::worldgen
