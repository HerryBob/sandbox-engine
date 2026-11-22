#include "modhost/entity/EntityApi.hpp"

#include "core/Log.hpp"
#include "modhost/entity/EntityDefTable.hpp"
#include "modhost/shared/HostContext.hpp"

#include <cstdint>
#include <sstream>
#include <string>

namespace engine::modhost {

// 模组给的资源路径按模组自己的 assets/ 或 data/ 根解析：
// "assets/..." / "data/..." → "mods/<模组名>/..."。其它写法原样保留（走引擎全局资源根）。
// 这样模组写路径不用关心自己装在哪个子目录。
static std::string resolveModResourcePath(const std::string& modName, const char* path) {
    if (path == nullptr || path[0] == '\0') return std::string();
    std::string p(path);
    const bool underModRoot = p.rfind("assets/", 0) == 0 || p.rfind("data/", 0) == 0;
    if (!underModRoot || modName.empty()) return p;
    return "mods/" + modName + "/" + p;
}

std::int32_t registerEntityImpl(void* host, const ModEntityDef* def) {
    if (host == nullptr || def == nullptr) return -1;
    if (def->name == nullptr || def->model == nullptr) {
        ENGINE_LOG_ERROR("Mod", "registerEntity 被拒绝：name / model 为空");
        return -1;
    }
    // 尺寸与速度必须是正常值：负数会让物理与相机算出无意义结果。
    if (def->width <= 0.0f || def->height <= 0.0f || def->depth <= 0.0f ||
        def->eyeHeight < 0.0f || def->moveSpeed < 0.0f || def->jumpSpeed < 0.0f) {
        ENGINE_LOG_ERROR("Mod", std::string("registerEntity 被拒绝：尺寸/速度为非法值（")
                  + def->name + "）");
        return -1;
    }

    HostContext* ctx = static_cast<HostContext*>(host);

    if (g_entityTypeRegistry.frozen()) {
        ENGINE_LOG_ERROR("Mod", std::string("registerEntity 被拒绝：实体类型表已冻结，加载顺序错误（")
                  + def->name + "）");
        return -1;
    }
    // 类型 id 上限 65534：给实例分配与调试留出可读范围。
    if (ctx->nextEntityTypeId > 65534) {
        ENGINE_LOG_ERROR("Mod", "registerEntity 被拒绝：实体类型 id 已用尽（上限 65535 个）");
        return -1;
    }

    EntityTypeInfo info{};
    info.name       = def->name;
    info.model      = resolveModResourcePath(ctx->currentMod, def->model);
    // texture 允许为空：空串表示"用模型自己寻址的贴图"。
    info.texture    = resolveModResourcePath(ctx->currentMod, def->texture);
    info.width      = def->width;
    info.height     = def->height;
    info.depth      = def->depth;
    info.eyeHeight  = def->eyeHeight;
    info.moveSpeed  = def->moveSpeed;
    info.jumpSpeed  = def->jumpSpeed;
    info.animUser   = def->animUser;
    info.onAnimate  = def->onAnimate;

    const std::int32_t id = ctx->nextEntityTypeId;

    if (!g_entityTypeRegistry.add(id, info, ctx->currentMod.c_str())) {
        ENGINE_LOG_ERROR("Mod", std::string("registerEntity 失败：id=") + std::to_string(id) +
                  " 已被占用（" + def->name + "）");
        return -1;
    }
    ctx->nextEntityTypeId = id + 1;

    std::ostringstream oss;
    oss << "实体类型注册：" << def->name << " → id=" << id
        << "（模型 " << info.model
        << "，贴图 " << (info.texture.empty() ? "模型自带" : info.texture)
        << "，碰撞箱 " << info.width << "×" << info.height << "×" << info.depth
        << "，眼高 " << info.eyeHeight
        << "，步速 " << info.moveSpeed
        << "，跳速 " << info.jumpSpeed
        << "，动画回调 " << (info.onAnimate != nullptr ? "有" : "无") << "）";
    ENGINE_LOG_INFO("Mod", oss.str());
    return id;
}

} // namespace engine::modhost
