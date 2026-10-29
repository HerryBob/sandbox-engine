#include "TextureAtlas.hpp"
#include "registry/BlockRegistry.hpp"
#include <set>
#include <cstring>
#include <cstdio>
#include <stb_image.h>

namespace engine::render {

namespace {
    // 填充一块 16² 棋盘格（MC 风格缺失纹理：洋红/黑，每 2px 一格）
    void fillChecker(std::uint8_t* dst) {
        for (std::uint32_t y = 0; y < kAtlasTile; ++y)
            for (std::uint32_t x = 0; x < kAtlasTile; ++x) {
                const bool magenta = (((x / 2) + (y / 2)) & 1u) == 0;
                std::uint8_t* p = dst + (y * kAtlasTile + x) * 4;
                p[0] = magenta ? 255u : 0u;   // R
                p[1] = 0u;                    // G
                p[2] = magenta ? 255u : 0u;   // B
                p[3] = 255u;                  // A
            }
    }
} // namespace

bool TextureAtlas::buildFromRegistry() {
    // ① 全图先填棋盘格（空槽默认 = 缺失纹理，后续逐格覆盖）
    pixels_.assign(kAtlasSize * kAtlasSize * 4, 0u);
    for (std::uint32_t r = 0; r < kAtlasGrid; ++r)
        for (std::uint32_t c = 0; c < kAtlasGrid; ++c)
            fillChecker(pixels_.data() + (r * kAtlasSize + c * kAtlasTile) * 4);

    // ② 收集注册表里的纹理名（去重；第 0 格恒为 default 棋盘格，纹理从第 1 格起填）
    std::set<std::string_view> seen;
    std::uint32_t slotIndex = 1;   // 第 0 格留给 default（缺失纹理兜底）
    for (const auto& [key, info] : engine::registry::g_blockRegistry) {
        (void)key;
        if (!info.texture) continue;
        const std::string_view name(info.texture);
        if (!seen.insert(name).second) continue;              // 去重：同名只填一次
        if (slotIndex >= kAtlasGrid * kAtlasGrid) {
            std::fprintf(stderr, "[Atlas] 纹理数超图集容量(%u×%u)，跳过后续\n",
                         kAtlasGrid, kAtlasGrid);
            break;
        }
        const AtlasSlot slot{
            static_cast<std::uint8_t>(slotIndex / kAtlasGrid),
            static_cast<std::uint8_t>(slotIndex % kAtlasGrid)};
        // 加载失败 → 格子保留棋盘格（缺文件兜底，不崩引擎）
        if (addFromFile(name, slot))
            slots_.emplace(std::string(name), slot);
        else
            std::fprintf(stderr, "[Atlas] 纹理缺失：%s → 该格用棋盘格\n",
                         std::string(name).c_str());
        ++slotIndex;
    }
    return !pixels_.empty();
}

bool TextureAtlas::addFromFile(std::string_view name, AtlasSlot slot) {
    const std::string path = std::string("textures/") + std::string(name) + ".png";
    int w = 0, h = 0, ch = 0;
    stbi_uc* src = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
    if (!src) return false;

    // 直拷模式：引擎不做任何缩放——源图尺寸必须等于格子尺寸（16²），原样进格。
    // 未来升像素：只需改 kAtlasTile（如 32）且源图同步 32²，此处零改动。
    if (w != static_cast<int>(kAtlasTile) || h != static_cast<int>(kAtlasTile)) {
        std::fprintf(stderr, "[Atlas] 纹理尺寸不符：%s 为 %dx%d（应为 %ux%u）→ 该格用棋盘格\n",
                     std::string(name).c_str(), w, h, kAtlasTile, kAtlasTile);
        stbi_image_free(src);
        return false;
    }

    // 直接写入图集格：源像素 → 格（源已是 16²，逐行拷贝）
    std::uint8_t* base = pixels_.data() + (slot.row * kAtlasSize + slot.col * kAtlasTile) * 4;
    for (std::uint32_t y = 0; y < kAtlasTile; ++y)
        std::memcpy(base + y * kAtlasSize * 4,   // 每行间距 = 图集宽 128（格内行间有空隙列）
                    src + y * kAtlasTile * 4, kAtlasTile * 4);
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

} // namespace engine::render
