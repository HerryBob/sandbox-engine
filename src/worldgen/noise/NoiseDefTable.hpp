#pragma once
// 噪声定义表：data/worldgen/noise/*.json 全量解码注册，供预设按名字引用。
// 对应 MC 的 RegistryDataLoader：启动时扫一次、解码一次，之后取用只查表。
#include "resource/DataRegistry.hpp"
#include "worldgen/noise/NoiseDef.hpp"

namespace engine::worldgen {

// 全局唯一的噪声定义表
extern resource::DataRegistry<NoiseDef> g_noiseDefs;

// 扫 data 根下的 worldgen/noise/*.json 并注册；须在预设加载之前调用
void loadNoiseDefs();

} // namespace engine::worldgen
