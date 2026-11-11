#pragma once

#include <cstdint>
#include <vector>

namespace engine::modhost {

// 一个实体实例：只记它属于哪个类型。
// 位姿/可见性归渲染侧的运行时状态（entityPose_），这里不重复存一份，
// 免得两处各写一半后对不上。
struct EntityInstance {
    std::int32_t typeId = -1;
};

// 下标即实例 id。0 号是占位哨兵，避免 0 被当成有效实例。
inline std::vector<EntityInstance> g_entityInstances;

// 启动时先占住 0 号，保证第一个真实实例 id 从 1 开始。
// 与 VulkanContext::loadEntityModel 的 entityId 是同一套编号。
inline void reserveEntityInstanceIdZero() {
    if (g_entityInstances.empty()) g_entityInstances.push_back(EntityInstance{});
}

// 按类型创建实例，返回实例 id；typeId 非法时返回 -1。
inline std::int32_t createEntityInstance(std::int32_t typeId) {
    if (typeId < 0) return -1;
    const std::int32_t id = static_cast<std::int32_t>(g_entityInstances.size());
    g_entityInstances.push_back(EntityInstance{typeId});
    return id;
}

// 查实例：id 越界返回 nullptr。
inline const EntityInstance* findEntityInstance(std::int32_t id) {
    if (id <= 0 || id >= static_cast<std::int32_t>(g_entityInstances.size())) return nullptr;
    return &g_entityInstances[static_cast<std::size_t>(id)];
}

// 已分配实例数（含 0 号哨兵）。
inline std::size_t entityInstanceCount() { return g_entityInstances.size(); }

// 销毁单个实例：把条目重置为"无类型"，不缩表 —— 实例 id 一旦给出就不再复用，
// 否则渲染侧按 id 索引的运行时状态会被新实例错误继承。
inline bool destroyEntityInstance(std::int32_t id) {
    if (id <= 0 || id >= static_cast<std::int32_t>(g_entityInstances.size())) return false;
    EntityInstance& inst = g_entityInstances[static_cast<std::size_t>(id)];
    if (inst.typeId < 0) return false;
    inst.typeId = -1;
    return true;
}

// 按类型批量销毁（模组卸载闭环）：返回被销毁的实例数。
inline std::size_t destroyEntityInstancesOfType(std::int32_t typeId) {
    if (typeId < 0) return 0U;
    std::size_t removed = 0;
    for (std::size_t i = 1; i < g_entityInstances.size(); ++i) {
        if (g_entityInstances[i].typeId != typeId) continue;
        g_entityInstances[i].typeId = -1;
        ++removed;
    }
    return removed;
}

// 存活实例数（typeId >= 0，不含 0 号哨兵与已销毁条目）。
inline std::size_t aliveEntityInstanceCount() {
    std::size_t alive = 0;
    for (std::size_t i = 1; i < g_entityInstances.size(); ++i)
        if (g_entityInstances[i].typeId >= 0) ++alive;
    return alive;
}

} // namespace engine::modhost
