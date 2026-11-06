#include "PerlinNoise.hpp"

#include "NoiseHash.hpp"

#include <cmath>

namespace mod {

PerlinNoise::PerlinNoise(double scale) : scale_(scale) {}

double PerlinNoise::fade(double t) {
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);   // 6t⁵ - 15t⁴ + 10t³
}

double PerlinNoise::sample(double x, double z, std::uint64_t seed) const {
    const double sx = x * scale_;
    const double sz = z * scale_;
    const int ix = static_cast<int>(std::floor(sx));
    const int iz = static_cast<int>(std::floor(sz));
    const double fx = sx - static_cast<double>(ix);     // [0,1)
    const double fz = sz - static_cast<double>(iz);

    // 四角梯度，与"p 到该角"的偏移向量做点积：
    // 每个角贡献一个"斜坡"，点积 = 该斜坡在 p 处的高度。
    double g00x, g00z; detail::latticeGradient(ix,     iz,     seed, g00x, g00z);
    double g10x, g10z; detail::latticeGradient(ix + 1, iz,     seed, g10x, g10z);
    double g01x, g01z; detail::latticeGradient(ix,     iz + 1, seed, g01x, g01z);
    double g11x, g11z; detail::latticeGradient(ix + 1, iz + 1, seed, g11x, g11z);
    const double d00 = g00x * fx + g00z * fz;
    const double d10 = g10x * (fx - 1.0) + g10z * fz;
    const double d01 = g01x * fx + g01z * (fz - 1.0);
    const double d11 = g11x * (fx - 1.0) + g11z * (fz - 1.0);

    // 平滑插值混合（与 value 相同的双线性骨架，质量更高的 fade）
    const double ux = fade(fx);
    const double uz = fade(fz);
    const double row0 = d00 + (d10 - d00) * ux;
    const double row1 = d01 + (d11 - d01) * ux;
    return row0 + (row1 - row0) * uz;
}

} // namespace mod
