#include "modhost/block/BlockApi.hpp"

#include "core/Log.hpp"
#include "modhost/shared/HostContext.hpp"
#include "registry/BlockRegistry.hpp"

#include <cmath>   // B3：std::isfinite 校验 mass/hardness/friction
#include <cstdint>
#include <sstream>
#include <string>

namespace engine::modhost {

std::int32_t registerBlockImpl(void* host, const ModBlockDef* def) {
    if (host == nullptr || def == nullptr) return -1;
    if (def->name == nullptr || def->texture == nullptr) {
        ENGINE_LOG_ERROR("Mod", "registerBlock 被拒绝：name / texture 为空");
        return -1;
    }

    HostContext* ctx = static_cast<HostContext*>(host);

    if (engine::registry::g_blockRegistry.frozen()) {
        ENGINE_LOG_ERROR("Mod", std::string("registerBlock 被拒绝：方块注册表已冻结，加载顺序错误（")
                  + def->name + "）");
        return -1;
    }
    // 上限 65534：id 0 保留给空气，0xFFFF 保留给"未注册"哨兵，中间全部可用。
    if (ctx->nextBlockId > 65534U) {
        ENGINE_LOG_ERROR("Mod", "registerBlock 被拒绝：方块 id 已用尽（上限 65534 个）");
        return -1;
    }

    // B3：数值字段必须有限且在合理范围 —— 模组可以传任意 float，
    // NaN/负数一旦进 Jolt 会污染质量与摩擦（表现为刚体瞬移或不滑）。
    if (!std::isfinite(def->mass) || def->mass <= 0.0f) {
        ENGINE_LOG_ERROR("Mod", std::string("registerBlock 被拒绝：mass 非法（需 > 0 的有限值，")
                  + def->name + "）");
        return -1;
    }
    if (!std::isfinite(def->hardness) || def->hardness < 0.0f) {
        ENGINE_LOG_ERROR("Mod", std::string("registerBlock 被拒绝：hardness 非法（需 >= 0，")
                  + def->name + "）");
        return -1;
    }
    if (!std::isfinite(def->friction) || def->friction < 0.0f || def->friction > 1.0f) {
        ENGINE_LOG_ERROR("Mod", std::string("registerBlock 被拒绝：friction 非法（需 0..1，")
                  + def->name + "）");
        return -1;
    }

    // B4：方块名必须唯一 —— 否则 findBlockIdByName 返回哪个 id 取决于哈希桶顺序，
    // 生成器引用 "grass" 时就可能拿到别的模组的同名方块。
    for (auto it = engine::registry::g_blockRegistry.begin();
         it != engine::registry::g_blockRegistry.end(); ++it) {
        if (it->second.name == def->name) {
            ENGINE_LOG_ERROR("Mod", std::string("registerBlock 被拒绝：方块名重复（")
                      + def->name + "）");
            return -1;
        }
    }

    engine::registry::BlockInfo info{};
    info.name       = def->name;
    // B59：不再写 opaque —— 面剔除改由 renderType 决定（BlockInfo 里已删除该字段）。
    // def->opaque 仍是 ABI 字段（规则7 接口不动），但引擎不读它。
    info.hardness   = def->hardness;
    info.renderType = static_cast<engine::registry::RenderType>(def->renderType);
    info.texture    = def->texture;
    // 注意：model 允许为 nullptr（大多数方块走默认立方体）。
    // std::string::operator=(const char*) 要求指针非空，nullptr 是未定义行为，必须显式兜底。
    info.model      = (def->model != nullptr) ? def->model : "";
    info.mass       = def->mass;
    info.solid      = (def->solid != 0U);
    info.friction   = def->friction;

    const std::uint16_t id = ctx->nextBlockId;

    if (!engine::registry::g_blockRegistry.add(id, info, ctx->currentMod.c_str())) {
        ENGINE_LOG_ERROR("Mod", std::string("registerBlock 失败：id=") +
                  std::to_string(static_cast<int>(id)) + " 已被占用（" + def->name + "）");
        return -1;
    }
    ctx->nextBlockId = static_cast<std::uint16_t>(id + 1U);

    std::ostringstream oss;
    oss << "方块注册：" << def->name << " → id=" << static_cast<int>(id)
        << "（纹理 " << def->texture << "，硬度 " << info.hardness
        << "，质量 " << info.mass
        << "，碰撞 " << (info.solid ? "是" : "否")
        << "，摩擦 " << info.friction << "）";
    ENGINE_LOG_INFO("Mod", oss.str());
    return static_cast<std::int32_t>(id);
}

} // namespace engine::modhost
