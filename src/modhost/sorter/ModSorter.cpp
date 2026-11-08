#include "modhost/sorter/ModSorter.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::modhost {

SortResult ModSorter::sort(const std::vector<ModManifest>& items) {
    SortResult result;

    const std::size_t n = items.size();
    std::vector<bool> rejectedFlag(n, false);
    std::vector<std::string> reason(n);

    // ---- 1. id 索引；同一 id 出现多次时，涉及的模组全部拒绝 ----
    std::unordered_map<std::string, std::size_t> byId;
    std::unordered_set<std::string> duplicateIds;

    for (std::size_t i = 0; i < n; ++i) {
        const std::string& id = items[i].id;

        if (duplicateIds.count(id) > 0) {
            rejectedFlag[i] = true;
            reason[i] = "id 重复：" + id;
            continue;
        }

        const auto it = byId.find(id);
        if (it == byId.end()) {
            byId.emplace(id, i);
            continue;
        }

        const std::size_t other = it->second;
        rejectedFlag[other] = true;
        reason[other] = "id 重复：" + id;
        rejectedFlag[i] = true;
        reason[i] = "id 重复：" + id;
        duplicateIds.insert(id);
        byId.erase(it);
    }

    // ---- 2. 反向表：被依赖的 id → 依赖它的下标列表 ----
    std::unordered_map<std::string, std::vector<std::size_t>> dependents;
    for (std::size_t i = 0; i < n; ++i) {
        for (const std::string& dep : items[i].dependencies) {
            dependents[dep].push_back(i);
        }
    }

    // ---- 3. 直接拒绝：依赖的 id 不存在，或依赖了更晚批次的模组 ----
    for (std::size_t i = 0; i < n; ++i) {
        if (rejectedFlag[i]) continue;

        for (const std::string& dep : items[i].dependencies) {
            const auto it = byId.find(dep);
            if (it == byId.end()) {
                rejectedFlag[i] = true;
                reason[i] = "依赖不存在：" + dep;
                break;
            }
            if (items[it->second].phase < items[i].phase) {
                rejectedFlag[i] = true;
                reason[i] = "依赖了更晚批次的模组：" + dep;
                break;
            }
        }
    }

    // ---- 4. 级联：依赖方跟着被拒绝 ----
    std::vector<std::size_t> cascade;
    for (std::size_t i = 0; i < n; ++i) {
        if (rejectedFlag[i]) cascade.push_back(i);
    }
    for (std::size_t head = 0; head < cascade.size(); ++head) {
        const std::size_t x = cascade[head];
        const auto it = dependents.find(items[x].id);
        if (it == dependents.end()) continue;

        for (std::size_t y : it->second) {
            if (rejectedFlag[y]) continue;
            rejectedFlag[y] = true;
            reason[y] = "依赖的模组被拒绝：" + items[x].id;
            cascade.push_back(y);
        }
    }

    // ---- 5. 按 phase 分组，组内 Kahn 拓扑排序 ----
    std::vector<ModPhase> phases;
    for (const ModManifest& m : items) {
        phases.push_back(m.phase);
    }
    std::sort(phases.begin(), phases.end());
    phases.erase(std::unique(phases.begin(), phases.end()), phases.end());

    for (ModPhase phase : phases) {
        std::vector<std::size_t> group;
        for (std::size_t i = 0; i < n; ++i) {
            if (rejectedFlag[i]) continue;
            if (items[i].phase != phase) continue;
            group.push_back(i);
        }
        if (group.empty()) continue;

        std::unordered_map<std::size_t, std::size_t> toLocal;
        for (std::size_t k = 0; k < group.size(); ++k) {
            toLocal.emplace(group[k], k);
        }

        // 入度只统计组内依赖；跨组依赖已在第 3 步校验过批次方向
        std::vector<std::size_t> inDegree(group.size(), 0);
        std::vector<std::vector<std::size_t>> next(group.size());

        for (std::size_t k = 0; k < group.size(); ++k) {
            const std::size_t global = group[k];
            for (const std::string& dep : items[global].dependencies) {
                const auto itId = byId.find(dep);
                if (itId == byId.end()) continue;
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
            result.order.push_back(group[k]);
            for (std::size_t nk : next[k]) {
                --inDegree[nk];
                if (inDegree[nk] == 0) queue.push_back(nk);
            }
        }

        // 没能出队的都在环上，或被环拖住
        if (queue.size() < group.size()) {
            for (std::size_t k = 0; k < group.size(); ++k) {
                if (inDegree[k] == 0) continue;
                rejectedFlag[group[k]] = true;
                reason[group[k]] = "循环依赖";
            }
        }
    }

    // ---- 6. 收集被拒绝的模组 ----
    for (std::size_t i = 0; i < n; ++i) {
        if (rejectedFlag[i]) {
            result.rejected.push_back(RejectedMod{i, reason[i]});
        }
    }
    return result;
}

} // namespace engine::modhost
