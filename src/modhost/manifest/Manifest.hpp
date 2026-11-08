#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace engine::modhost {

// 模组加载阶段；整数值即排序依据（先按 phase 选组，组内再做依赖拓扑排序）。
enum class ModPhase : std::int32_t {
    Builtin = 0,   // 内置内容：方块注册 id 须稳定在 1..N，整体先于其它模组加载完
    Normal  = 1,   // 普通模组
};

// 模组清单（mod.json）：加载前读入，用于排序与拒绝。
// 排序、拒绝、依赖解析以清单为准；显示名以 IMod::name() 为准，两者不一致记警告但不拒绝。
struct ModManifest {
    std::string id;                          // 唯一键（被其它模组的 dependencies 引用）
    ModPhase    phase = ModPhase::Normal;    // 加载批次，缺省 normal
    std::vector<std::string> dependencies;   // 依赖的模组 id，缺省为空
};

// 清单读取器：<与 DLL 同名同目录>.json → ModManifest。
// 只校验格式；依赖 id 是否真实存在归加载流程检查。
class ManifestReader {
public:
    // path：清单文件完整路径（如 "mods\\blocks.json"）
    // out ：解析结果；失败时不改动（不产出半成品）
    // 返回：全部字段合法为 true；语法错、id 缺失或非法、phase 非法、dependencies 非法一律 false
    static bool load(const std::string& path, ModManifest& out);
};

} // namespace engine::modhost
