#pragma once

#include "NoiseSource.hpp"

#include <memory>

namespace mod {

// fbm（分形布朗运动）：组合子 —— 对基底噪声多八度叠加。
// 每层频率 × lacunarity、振幅 × gain，层层求和 —— 用组合代替新算法，
// 让平滑的基底噪声长出自然的山脉细节。
// 基底：任何 NoiseSource（perlin/value/模组噪声皆可），所有权移交本对象。
class FbmNoise : public NoiseSource {
public:
    FbmNoise(std::unique_ptr<NoiseSource> base,
             int octaves, double scale, double lacunarity, double gain);

    double sample(double x, double z, std::uint64_t seed) const override;

private:
    std::unique_ptr<NoiseSource> base_;   // 被叠加的基底噪声（独占所有权）
    int octaves_ = 4;                     // 叠加层数
    double scale_ = 1.0;                  // 整体采样频率（第 0 层的频率）
    double lacunarity_ = 2.0;             // 频率倍率（每层频率 × 多少）
    double gain_ = 0.5;                   // 振幅衰减（每层振幅 × 多少）
};

} // namespace mod
