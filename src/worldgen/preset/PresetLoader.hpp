#pragma once

#include "worldgen/preset/WorldPreset.hpp"

#include <string>

namespace engine::worldgen {

// 世界预设加载器：data/worldgen/presets/<name>.json → WorldPreset（噪声已实例化）。
// 纪律：关键键（seed / heightmap.noise.type / baseHeight / amplitude / strata）
// 缺失或非法一律失败（stderr 说明），不静默补默认；非关键数值键走 NoiseParams.values
// （引擎不认识参数键——模组参数面的入口，拼错由各实现自行兜底）。
// 调用时机：内置噪声注册之后即可（make 不受 freeze 限制，freeze 只拒绝注册）。
class PresetLoader {
public:
    // name：预设名（不带 .json），如 "default"
    // 失败返回 false 且不改动 out（不产出半成品）
    static bool load(const std::string& name, WorldPreset& out);
};

} // namespace engine::worldgen
