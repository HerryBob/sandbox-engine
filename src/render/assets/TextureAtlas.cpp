#include "TextureAtlas.hpp"
#include "registry/BlockRegistry.hpp"
#include "render/assets/ModelRegistry.hpp"
#include <set>
#include <cstring>
#include <cstdio>
#include <stb_image.h>

namespace engine::assets {

bool TextureAtlas::buildFromRegistry() {
    // ① 分配像素数组（全 0 透明黑兜底，正常流程会立即被 default.png 覆盖）
    pixels_.assign(kAtlasSize * kAtlasSize * kAtlasChannels, 0u);

    // ② 第 0 格 = default.png（缺失纹理兜底，内容来自文件而非程序生成）
    if (!addFromFile("default", AtlasSlot{0, 0}))
        std::fprintf(stderr, "[Atlas] 兜底纹理 default.png 缺失 → 第 0 格透明黑\n");

    // ③ 收集纹理名（去重；第 0 格恒为 default，纹理从第 1 格起填）
    //    来源一：方块注册表的兜底纹理（BlockInfo::texture）
    //    来源二：模型注册表引用的纹理（ModelDef::textures 变量表的值）
    //            —— 模型面的纹理名也要进图集，否则模型引用会落到兜底格
    std::set<std::string_view> seen;
    std::uint32_t slotIndex = 1;   // 第 0 格留给 default（缺失纹理兜底）
    const auto fillSlot = [&](std::string_view name) {
        if (slotIndex >= kAtlasGrid * kAtlasGrid) {
            std::fprintf(stderr, "[Atlas] 纹理数超图集容量(%u×%u)，跳过后续\n",
                         kAtlasGrid, kAtlasGrid);
            return false;
        }
        const AtlasSlot slot{
            static_cast<std::uint8_t>(slotIndex / kAtlasGrid),
            static_cast<std::uint8_t>(slotIndex % kAtlasGrid)};
        // 加载失败 → 格子留空（透明黑），不崩引擎
        if (addFromFile(name, slot))
            slots_.emplace(std::string(name), slot);
        else
            std::fprintf(stderr, "[Atlas] 纹理缺失：%s → 该格透明黑\n",
                         std::string(name).c_str());
        ++slotIndex;
        return true;
    };

    for (const auto& [key, info] : engine::registry::g_blockRegistry) {
        (void)key;
        if (!info.texture) continue;
        const std::string_view name(info.texture);
        if (!seen.insert(name).second) continue;              // 去重：同名只填一次
        if (!fillSlot(name)) break;
    }
    for (const auto& [modelName, def] : engine::assets::g_modelRegistry) {
        (void)modelName;
        for (const auto& [var, tex] : def.textures) {
            (void)var;
            const std::string_view name(tex);
            if (!seen.insert(name).second) continue;
            if (!fillSlot(name)) break;
        }
    }
    return !pixels_.empty();
}

bool TextureAtlas::addFromFile(std::string_view name, AtlasSlot slot) {
    const std::string path = std::string("assets/textures/block/") + std::string(name) + ".png";
    int w = 0, h = 0, ch = 0;
    stbi_uc* src = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
    if (!src) return false;

    // 直拷模式：引擎不做任何缩放——源图尺寸必须等于格子尺寸（16²），原样进格。
    // 未来升像素：只需改 kAtlasTile（如 32）且源图同步 32²，此处零改动。
    if (w != static_cast<int>(kAtlasTile) || h != static_cast<int>(kAtlasTile)) {
        std::fprintf(stderr, "[Atlas] 纹理尺寸不符：%s 为 %dx%d（应为 %ux%u）→ 该格透明黑\n",
                     std::string(name).c_str(), w, h, kAtlasTile, kAtlasTile);
        stbi_image_free(src);
        return false;
    }

    // 直接写入图集格：源像素 → 格（源已是 16²，逐行拷贝）
    std::uint8_t* base = pixels_.data()  + (slot.row * kAtlasSize + slot.col * kAtlasTile) * kAtlasChannels;
    for (std::uint32_t y = 0; y < kAtlasTile; ++y)
        std::memcpy(base + y * kAtlasSize * kAtlasChannels,   // 每行间距 = 图集宽 128（格内行间有空隙列）
                    src + y * kAtlasTile * kAtlasChannels, 
                    kAtlasTile * kAtlasChannels);
    stbi_image_free(src);
    return true;
}

bool TextureAtlas::slotOf(std::string_view name, AtlasSlot& out) const {
    const auto it = slots_.find(std::string(name));
    if (it == slots_.end()) return false;
    out = it->second;
    return true;
}

glm::vec4 TextureAtlas::slotUV(AtlasSlot s) {
    const float f = 1.0f / static_cast<float>(kAtlasGrid);
    return { s.col * f, s.row * f, (s.col + 1) * f, (s.row + 1) * f };
}

} // namespace engine::assets
