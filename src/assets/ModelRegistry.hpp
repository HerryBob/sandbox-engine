#pragma once
// 模型注册表：模型名（"wood"）→ ModelDef（Key = std::string，复用 Registry 模板）。
// 与方块注册表同款生命周期：add（加载）→ freeze（锁表，免锁读）。
#include "assets/ModelDef.hpp"
#include "registry/Registry.hpp"

namespace engine::assets {
    inline engine::registry::Registry<ModelDef, std::string> g_modelRegistry;
}
