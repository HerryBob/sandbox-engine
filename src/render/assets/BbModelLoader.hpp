#pragma once
// bbmodel 加载器：Blockbench 5.0 导出文件 → EntityModel。
// 只读 elements（cube）与 textures；有 groups 就按 uuid 建父子，没有就按骨骼名约定。
#include "render/assets/EntityModel.hpp"
#include <string>

namespace engine::assets {

// 解析一个 bbmodel 文件并归一化到「脚底中心原点」。
// 返回 false 表示读不到文件或结构不可用（原因写日志）。
bool loadBbModel(const std::string& path, EntityModel& out);

} // namespace engine::assets
