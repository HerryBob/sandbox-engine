#pragma once
// 噪声定义（纯数据）：data/worldgen/noise/<name>.json 解码出来的内容。
// 只描述"调哪个 provider、传什么参数"，不含已实例化的噪声对象 ——
// 实例化在 ColumnGenerator::prepare() 里做，那时 provider 必然已注册。
#include "worldgen/preset/WorldPreset.hpp"

#include <string>
#include <vector>

#include <rapidjson/document.h>

namespace engine::worldgen {

struct NoiseDef {
    std::string type;                  // provider 名（"flat" / "perlin" / "fbm"）
    std::string base;                  // 嵌套基底（fbm 用；空 = 无）
    std::vector<HeightParam> params;   // 其余参数，数值或字符串二选一
};

// 一段 JSON（文件内容或预设内联对象）→ NoiseDef；缺 type 或非对象返回 false，不改 out。
// rel 是该条所在路径（内联来源传说明串），仅用于日志。
bool decodeNoiseDef(const rapidjson::Value& v, NoiseDef& out, const std::string& rel);

} // namespace engine::worldgen
