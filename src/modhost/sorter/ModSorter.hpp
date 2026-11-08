#pragma once

#include "modhost/manifest/Manifest.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace engine::modhost {

// 被拒绝的模组
struct RejectedMod {
    std::size_t index;    // 在输入数组中的下标
    std::string reason;   // 拒绝原因（写日志用）
};

// 排序结果
struct SortResult {
    std::vector<std::size_t> order;     // 可加载的模组下标，按加载顺序排列
    std::vector<RejectedMod> rejected;  // 被拒绝的模组及原因
};

// 依赖排序器：按 phase 分组，组内用 Kahn 算法（入度 + 队列）做拓扑排序。
// 拒绝情形：id 重复（双方都拒绝）、依赖的 id 不存在、依赖了更晚批次的模组、
//          依赖链上有人被拒绝（级联）、组内存在环（环上及被环拖住的一并拒绝）。
class ModSorter {
public:
    // items：待排序的模组清单；调用方应已按 (phase, id) 排好，保证同层顺序稳定
    // 返回：order 为可加载下标序列，rejected 为被拒绝的下标与原因
    static SortResult sort(const std::vector<ModManifest>& items);
};

} // namespace engine::modhost
