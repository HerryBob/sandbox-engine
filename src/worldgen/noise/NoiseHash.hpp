#pragma once

#include <cstdint>

namespace engine::worldgen::detail {

// 内部实现细节：格点伪随机（世界生成确定性基石——同输入永远同输出）。
// 只被具体噪声实现使用，不对外暴露（不在 NoiseSource 契约里）。

// 整数格点坐标 + seed → 64 位伪随机散列（splitmix64 风格混合）。
// 坐标可为负：先转无符号（模运算回绕，定义良好），再乘大质数混合。
inline std::uint64_t hashLattice(int ix, int iz, std::uint64_t seed) {
    std::uint64_t h = seed;
    h ^= static_cast<std::uint64_t>(ix) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<std::uint64_t>(iz) * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
}

// 格点随机标量（value 噪声用）：散列 → [-1, 1]。
inline double latticeRandom(int ix, int iz, std::uint64_t seed) {
    const std::uint64_t h = hashLattice(ix, iz, seed);
    const double unit = static_cast<double>(h & 0xFFFFFFFFFFFFull)
                      / static_cast<double>(0xFFFFFFFFFFFFull);   // [0, 1)
    return unit * 2.0 - 1.0;                                      // [-1, 1]
}

// 格点随机梯度（perlin 噪声用）：散列 → 随机角度 → 单位方向向量。
inline void latticeGradient(int ix, int iz, std::uint64_t seed,
                            double& gx, double& gz) {
    const std::uint64_t h = hashLattice(ix, iz, seed);
    const double unit = static_cast<double>(h & 0xFFFFFFFFFFFFull)
                      / static_cast<double>(0xFFFFFFFFFFFFull);   // [0, 1)
    const double angle = unit * 2.0 * 3.14159265358979323846;     // [0, 2π)
    gx = std::cos(angle);
    gz = std::sin(angle);
}

} // namespace engine::worldgen::detail
