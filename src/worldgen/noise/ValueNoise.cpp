#include "worldgen/noise/ValueNoise.hpp"

#include "worldgen/noise/NoiseHash.hpp"

#include <cmath>

namespace engine::worldgen {

ValueNoise::ValueNoise(double scale) : scale_(scale) {}

double ValueNoise::fade(double t) {
    return t * t * (3.0 - 2.0 * t);   // 3t² - 2t³
}

double ValueNoise::sample(double x, double z, std::uint64_t seed) const {
    // 世界坐标 → 格点坐标（scale 缩放）
    const double sx = x * scale_;
    const double sz = z * scale_;
    const int ix = static_cast<int>(std::floor(sx));
    const int iz = static_cast<int>(std::floor(sz));
    const double fx = sx - static_cast<double>(ix);   // [0,1)：格内偏移
    const double fz = sz - static_cast<double>(iz);

    // 周围 2×2 格点的随机值（确定性：同格点同 seed 永远同值）
    const double v00 = detail::latticeRandom(ix,     iz,     seed);
    const double v10 = detail::latticeRandom(ix + 1, iz,     seed);
    const double v01 = detail::latticeRandom(ix,     iz + 1, seed);
    const double v11 = detail::latticeRandom(ix + 1, iz + 1, seed);

    // 平滑后双线性插值
    const double ux = fade(fx);
    const double uz = fade(fz);
    const double row0 = v00 + (v10 - v00) * ux;   // 下边两点
    const double row1 = v01 + (v11 - v01) * ux;   // 上边两点
    return row0 + (row1 - row0) * uz;
}

} // namespace engine::worldgen
