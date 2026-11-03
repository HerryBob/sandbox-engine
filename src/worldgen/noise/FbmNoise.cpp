#include "worldgen/noise/FbmNoise.hpp"

#include <utility>

namespace engine::worldgen {

FbmNoise::FbmNoise(std::unique_ptr<NoiseSource> base,
                   int octaves, double scale, double lacunarity, double gain)
    : base_(std::move(base)),
      octaves_(octaves),
      scale_(scale),
      lacunarity_(lacunarity),
      gain_(gain) {}

double FbmNoise::sample(double x, double z, std::uint64_t seed) const {
    double sum = 0.0;
    double amplitude = 1.0;
    double frequency = scale_;
    double amplitudeSum = 0.0;   // 归一化分母：各层振幅之和
    for (int i = 0; i < octaves_; ++i) {
        sum += amplitude * base_->sample(x * frequency, z * frequency, seed);
        amplitudeSum += amplitude;
        amplitude *= gain_;
        frequency *= lacunarity_;
    }
    // 归一化到 [-1,1]：除以总振幅（gain<1 时低层权重更大，山体骨架为主）
    return amplitudeSum > 0.0 ? sum / amplitudeSum : 0.0;
}

} // namespace engine::worldgen
