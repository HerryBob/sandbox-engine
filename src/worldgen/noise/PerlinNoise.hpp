#pragma once

#include "worldgen/noise/NoiseSource.hpp"

namespace engine::worldgen {

// 柏林噪声（perlin noise）：梯度噪声 —— 格点随机梯度 + 点积 + 平滑插值。
// 地形主力：连续、值域固定（理论 ±√2/2）、无方向偏好、确定性。
// 与 value 噪声的区别：格点放的是"方向（梯度）"而非"标量"，
// 点积产生更平滑的倾斜面，消除 value 的块状感。
class PerlinNoise : public NoiseSource {
public:
    explicit PerlinNoise(double scale);   // scale：采样频率（越大细节越密）

    double sample(double x, double z, std::uint64_t seed) const override;

private:
    double scale_ = 1.0;
    static double fade(double t);        // 高质量平滑 6t⁵-15t⁴+10t³
};

} // namespace engine::worldgen
