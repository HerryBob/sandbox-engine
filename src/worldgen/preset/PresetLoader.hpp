#pragma once

#include "worldgen/preset/WorldPreset.hpp"

#include <string>

namespace engine::worldgen {

// 世界预设加载器：data/worldgen/presets/<name>.json → WorldPreset（纯数据）。
// 纪律：关键键（seed / heightmap.noise.type / baseHeight / amplitude / strata）
// 缺失或非法一律失败（stderr 说明），不静默补默认；非关键数值键走 heightParams
// （引擎不认识参数键 —— 模组参数面的入口，拼错由各 provider 自行兜底）。
// 调用时机：所有模组加载完、注册表冻结之后（本类只做 contains 校验，不实例化；
// 实例化发生在 ColumnGenerator::prepare()）。
class PresetLoader {
public:
    // name：预设名（不带 .json），如 "default"
    // 失败返回 false 且不改动 out（不产出半成品）
    static bool load(const std::string& name, WorldPreset& out);
};

} // namespace engine::worldgen
