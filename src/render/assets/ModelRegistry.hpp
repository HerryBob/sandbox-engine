#pragma once
// 模型注册表：模型名（"wood"）→ ModelDef。
// 走统一注册器，与方块/贴图同一套「扫目录 + 全量解码 + 先到先得」规则。
#include "render/assets/ModelDef.hpp"
#include "resource/DataRegistry.hpp"

namespace engine::assets {
    inline engine::resource::DataRegistry<ModelDef> g_modelRegistry;
}
