#pragma once

#include "worldgen/noise/NoiseSource.hpp"

namespace engine::worldgen {

// 值噪声（value noise）：最简噪声 —— 整数格点随机值 + 平滑插值。
// 教学跳板：先跑通"接口→实现→注册→查表"全链，再上梯度噪声（perlin）。
// 特点：连续、确定性、实现直观；视觉上有轻微"块状"（不如 perlin 平滑）。
class ValueNoise : public NoiseSource {
public:
    explicit ValueNoise(double scale);   // scale：采样频率（越大细节越密）

    double sample(double x, double z, std::uint64_t seed) const override;

private:
    double scale_ = 1.0;
    static double fade(double t);        // 平滑曲线 3t²-2t³（端点导数 0）
};

} // namespace engine::worldgen
