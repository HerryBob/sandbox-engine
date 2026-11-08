#pragma once

#include "modapi/noise_api.h"

#include <cstdint>

namespace engine::modhost {

// 模组注册噪声 provider：转给 worldgen 的噪声注册表。
// host：加载上下文（模组回传的 void*），用来取当前模组名做来源标记
// def ：模组给出的 provider 描述
// 返回：成功 0；type 非法、重复或注册表已冻结时为 -1
std::int32_t registerNoiseImpl(void* host, const modapi::NoiseProviderDef* def);

} // namespace engine::modhost
