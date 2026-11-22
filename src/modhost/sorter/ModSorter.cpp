#include "modhost/sorter/ModSorter.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::modhost {
namespace {

// ============================================================================
// 排序过程的共享状态（规则1：原 sort 的局部变量按职责集中到这里）
// ============================================================================
struct SortState {
    explicit SortState(const std::vector<ModManifest>& src)
        : items(src), rejected(src.size(), false), reason(src.size()) {}

    const std::vector<ModManifest>& items;
    std::vector<bool>        rejected;   // 每个下标的拒绝标记
    std::vector<std::string> reason;     // 拒绝原因（仅在 rejected[i] 为真时有效）
    std::unordered_map<std::string, std::size_t> byId;    // id → 下标（去重后的幸存者）
    std::unordered_set<std::string>              duplicateIds;   // 已判定重复的 id
    std::unordered_map<std::string, std::vector<std::size_t>> dependents;  // 被依赖 id → 依赖它的下标
};

void reject(SortState& s, std::size_t i, const std::string& why) {
    s.rejected[i] = true;
    s.reason[i] = why;
}

// ① id 索引；同一 id 出现多次时，涉及的模组**全部**拒绝（谁也不知道该用哪个）
void indexIds(SortState& s) {
    const std::size_t n = s.items.size();
    for (std::size_t i = 0; i < n; ++i) {
        const std::string& id = s.items[i].id;

        if (s.duplicateIds.count(id) > 0) {
            reject(s, i, "id 重复：" + id);
            continue;
        }

        const auto it = s.byId.find(id);
        if (it == s.byId.end()) {
            s.byId.emplace(id, i);
            continue;
        }

        const std::size_t other = it->second;
        reject(s, other, "id 重复：" + id);
        reject(s, i, "id 重复：" + id);
        s.duplicateIds.insert(id);
        s.byId.erase(it);
    }
}

// ② 反向表：被依赖的 id → 依赖它的下标列表
void buildDependents(SortState& s) {
    for (std::size_t i = 0; i < s.items.size(); ++i) {
        for (const std::string& dep : s.items[i].dependencies) {
            s.dependents[dep].push_back(i);
        }
    }
}

// ③ 直接拒绝：依赖的 id 不存在，或依赖了更晚批次的模组
void rejectUnresolvableDeps(SortState& s) {
    for (std::size_t i = 0; i < s.items.size(); ++i) {
        if (s.rejected[i]) continue;

        for (const std::string& dep : s.items[i].dependencies) {
            const auto it = s.byId.find(dep);
            if (it == s.byId.end()) {
                reject(s, i, "依赖不存在：" + dep);
                break;
            }
            // 被依赖方的批次晚于自己 → 它在自己之后才加载，依赖拿不到
            if (s.items[it->second].phase > s.items[i].phase) {
                reject(s, i, "依赖了更晚批次的模组：" + dep);
                break;
            }
        }
    }
}

// ④ 级联：依赖方跟着被拒绝（BFS；同一节点只入队一次）
void cascadeRejections(SortState& s) {
    std::vector<std::size_t> cascade;
    for (std::size_t i = 0; i < s.items.size(); ++i) {
        if (s.rejected[i]) cascade.push_back(i);
    }
    for (std::size_t head = 0; head < cascade.size(); ++head) {
        const std::size_t x = cascade[head];
        const auto it = s.dependents.find(s.items[x].id);
        if (it == s.dependents.end()) continue;

        for (std::size_t y : it->second) {
            if (s.rejected[y]) continue;
            reject(s, y, "依赖的模组被拒绝：" + s.items[x].id);
            cascade.push_back(y);
        }
    }
}

// ⑤ 按 phase 分组，组内 Kahn 拓扑排序；可加载下标按顺序追加到 order。
//    跨组依赖已在第 ③ 步校验过批次方向，这里只统计组内入度。
void appendTopologicalOrder(SortState& s, std::vector<std::size_t>& order) {
    const std::size_t n = s.items.size();

    std::vector<ModPhase> phases;
    for (const ModManifest& m : s.items) phases.push_back(m.phase);
    std::sort(phases.begin(), phases.end());
    phases.erase(std::unique(phases.begin(), phases.end()), phases.end());

    for (ModPhase phase : phases) {
        std::vector<std::size_t> group;
        for (std::size_t i = 0; i < n; ++i) {
            if (s.rejected[i]) continue;
            if (s.items[i].phase != phase) continue;
            group.push_back(i);
        }
        if (group.empty()) continue;

        std::unordered_map<std::size_t, std::size_t> toLocal;
        for (std::size_t k = 0; k < group.size(); ++k) {
            toLocal.emplace(group[k], k);
        }

        std::vector<std::size_t> inDegree(group.size(), 0);
        std::vector<std::vector<std::size_t>> next(group.size());

        for (std::size_t k = 0; k < group.size(); ++k) {
            const std::size_t global = group[k];
            for (const std::string& dep : s.items[global].dependencies) {
                const auto itId = s.byId.find(dep);
                if (itId == s.byId.end()) continue;
                const auto itLocal = toLocal.find(itId->second);
                if (itLocal == toLocal.end()) continue;   // 跨组依赖，不计入组内入度

                ++inDegree[k];
                next[itLocal->second].push_back(k);
            }
        }

        std::vector<std::size_t> queue;
        for (std::size_t k = 0; k < group.size(); ++k) {
            if (inDegree[k] == 0) queue.push_back(k);
        }

        for (std::size_t head = 0; head < queue.size(); ++head) {
            const std::size_t k = queue[head];
            order.push_back(group[k]);
            for (std::size_t nk : next[k]) {
                --inDegree[nk];
                if (inDegree[nk] == 0) queue.push_back(nk);
            }
        }

        // 没能出队的都在环上，或被环拖住
        if (queue.size() < group.size()) {
            for (std::size_t k = 0; k < group.size(); ++k) {
                if (inDegree[k] == 0) continue;
                reject(s, group[k], "循环依赖");
            }
            // B2：环上模组被标拒之后**必须再级联一次**。第一次级联（函数开头）发生在
            // 环检测之前，覆盖不到"依赖环上模组"的这批 —— 它们会留在 order 里却没有依赖，
            // 而 ModLoader 的 failedIds 兜不住（被拒模组根本不进 order）。
            cascadeRejections(s);
        }
    }
}

// ⑥ 收集被拒绝的模组（按原下标顺序）
void collectRejected(const SortState& s, std::vector<RejectedMod>& out) {
    for (std::size_t i = 0; i < s.items.size(); ++i) {
        if (s.rejected[i]) out.push_back(RejectedMod{i, s.reason[i]});
    }
}

} // namespace

SortResult ModSorter::sort(const std::vector<ModManifest>& items) {
    SortResult result;
    SortState s(items);

    indexIds(s);                    // ① 去重
    buildDependents(s);             // ② 反向表
    rejectUnresolvableDeps(s);      // ③ 缺失 / 更晚批次
    cascadeRejections(s);           // ④ 级联
    appendTopologicalOrder(s, result.order);   // ⑤ 分组 + Kahn
    collectRejected(s, result.rejected);       // ⑥ 收集

    return result;
}

} // namespace engine::modhost
