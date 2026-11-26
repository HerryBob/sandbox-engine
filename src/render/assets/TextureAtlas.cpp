#include "TextureAtlas.hpp"
#include "core/Log.hpp"
#include "registry/BlockRegistry.hpp"
#include "render/assets/ModelRegistry.hpp"
#include "resource/ResourceIndex.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <stb_image.h>

namespace engine::assets {

std::vector<std::string> TextureAtlas::collectUsedTextures() const {
    // 方块引用了别的模组提供的贴图：该模组没带全资源，或两个模组用了同一个纹理名。
    // 引擎（owner 为空串）或引擎自带的贴图不报 —— 公共基础设施。
    const auto warnCrossOwner = [](std::string_view texName, const std::string& blockOwner) {
        if (blockOwner.empty()) return;
        const std::string texOwner =
            resource::g_resourceIndex.ownerOf(resource::ResourceKind::Assets,
                                              "textures/block/" + std::string(texName) + ".png");
        if (texOwner.empty() || texOwner == "engine" || texOwner == blockOwner) return;
        ENGINE_LOG_WARN("Atlas", "贴图跨模组引用：" + std::string(texName) + " 由 " + texOwner +
                          " 提供，方块（" + blockOwner + "）正在使用");
    };

    // 方块纹理名：先按方块 id 升序收集。注册表底层是 unordered_map，直接遍历的顺序
    // 不可复现；格序要求每次启动一致，所以排好 id 再入表。
    std::vector<std::pair<std::uint16_t, std::string>> byId;
    for (const auto& [id, info] : engine::registry::g_blockRegistry) {
        if (info.texture.empty()) continue;
        if (info.texture == "default") continue;              // 第 0 格固定给它
        byId.emplace_back(id, info.texture);
    }
    std::sort(byId.begin(), byId.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<std::string> used;
    std::set<std::string> uniq;
    for (const auto& entry : byId) {
        if (!uniq.insert(entry.second).second) continue;      // 去重：同名只填一次
        warnCrossOwner(entry.second, engine::registry::g_blockRegistry.ownerOf(entry.first));
        used.push_back(entry.second);
    }

    // 模型纹理槽位（g_modelRegistry 底层是 std::map，字典序确定）：
    // 补方块层没引用到的槽位，如原木顶面 log_top、草方块侧面 grass_side。
    for (const auto& [modelName, entry] : engine::assets::g_modelRegistry) {
        (void)modelName;
        for (const auto& [var, tex] : entry.value.textures) {
            (void)var;
            if (tex.empty() || tex == "default") continue;    // 第 0 格固定给它
            if (!uniq.insert(tex).second) continue;
            used.push_back(tex);
        }
    }
    return used;
}

void TextureAtlas::layoutGrid(std::size_t count) {
    // 每边格数：能放下「default + 纹理条目」的最小方阵
    const std::uint32_t need = 1 + static_cast<std::uint32_t>(count);
    grid_ = 1;
    while (grid_ * grid_ < need) ++grid_;
    size_ = kAtlasTile * grid_;
    pixels_.assign(static_cast<std::size_t>(size_) * size_ * kAtlasChannels, 0u);
}

bool TextureAtlas::placeDefaultSlot() {
    // 第 0 格 = default.png（缺失纹理兜底，内容来自文件而非程序生成）
    return addFromFile("default", AtlasSlot{0, 0});
}

TextureAtlas::Placement TextureAtlas::placeSlots(const std::vector<std::string>& names) {
    Placement p;
    // 按收集顺序铺格（第 0 格已被 default 占用，从第 1 格起）
    for (const std::string& name : names) {
        if (p.occupied >= grid_ * grid_) break;                   // 容量已满
        const AtlasSlot slot{
            static_cast<std::uint8_t>(p.occupied / grid_),
            static_cast<std::uint8_t>(p.occupied % grid_)};
        // 失败原因由 addFromFile 打（含提供者），这里只计数
        if (addFromFile(name, slot)) {
            slots_.emplace(name, slot);
            ++p.loaded;
        } else {
            ++p.failed;
        }
        ++p.occupied;
    }
    return p;
}

bool TextureAtlas::build() {
    const std::vector<std::string> used = collectUsedTextures();   // ① 自动收集（顺序确定）

    layoutGrid(used.size());                                       // ② 格数 + 像素缓冲

    if (!placeDefaultSlot())                                       // ③ 第 0 格 = default.png
        ENGINE_LOG_WARN("Atlas", "兜底纹理 default.png 缺失 → 第 0 格透明黑");

    const Placement p = placeSlots(used);                          // ④ 按收集顺序铺格

    ENGINE_LOG_INFO("Atlas", "图集 " + std::to_string(size_) + "²（" + std::to_string(grid_) + "×" +
                      std::to_string(grid_) + " 格）：自动 " + std::to_string(used.size()) +
                      " 张，成功 " + std::to_string(p.loaded) + " 张，失败 " +
                      std::to_string(p.failed) + " 张，占用 " + std::to_string(p.occupied) + "/" +
                      std::to_string(grid_ * grid_) + " 格");
    return !pixels_.empty();
}

bool TextureAtlas::addFromFile(std::string_view name, AtlasSlot slot) {
    // 走资源索引：引擎与各模组提供的 textures/block/*.png 合并在同一张表里，引擎优先
    const std::string rel = "textures/block/" + std::string(name) + ".png";
    std::filesystem::path path;
    if (!resource::g_resourceIndex.find(resource::ResourceKind::Assets, rel, path)) {
        ENGINE_LOG_WARN("Atlas", "纹理未找到：" + rel + "（没有任何资源根提供）→ 该格透明黑");
        return false;
    }
    const std::string owner =
        resource::g_resourceIndex.ownerOf(resource::ResourceKind::Assets, rel);

    int w = 0, h = 0, ch = 0;
    const std::string pathStr = path.string();
    stbi_uc* src = stbi_load(pathStr.c_str(), &w, &h, &ch, STBI_rgb_alpha);
    if (!src) {
        ENGINE_LOG_WARN("Atlas", "纹理解码失败：" + rel + "（由 " + owner + " 提供）→ 该格透明黑");
        return false;
    }

    // 直拷模式：引擎不做任何缩放——源图尺寸必须等于格子尺寸（16²），原样进格。
    // 未来升像素：只需改 kAtlasTile（如 32）且源图同步 32²，此处零改动。
    if (w != static_cast<int>(kAtlasTile) || h != static_cast<int>(kAtlasTile)) {
        ENGINE_LOG_WARN("Atlas", "纹理尺寸不符：" + rel + "（由 " + owner + " 提供）为 " +
                          std::to_string(w) + "x" + std::to_string(h) + "（应为 " +
                          std::to_string(kAtlasTile) + "x" + std::to_string(kAtlasTile) +
                          "）→ 该格透明黑");
        stbi_image_free(src);
        return false;
    }

    // 直接写入图集格：源像素 → 格（源已是 16²，逐行拷贝）
    // 偏移按像素算：格左上角 = (格行 × 格高, 格列 × 格宽)，再换算成字节
    const std::size_t pixelRow = static_cast<std::size_t>(slot.row) * kAtlasTile;
    const std::size_t pixelCol = static_cast<std::size_t>(slot.col) * kAtlasTile;
    std::uint8_t* base = pixels_.data() + (pixelRow * size_ + pixelCol) * kAtlasChannels;
    for (std::uint32_t y = 0; y < kAtlasTile; ++y)
        std::memcpy(base + y * size_ * kAtlasChannels,   // 每行间距 = 图集宽度（格内行间有空隙列）
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

glm::vec4 atlasSlotUVPure(std::uint32_t grid, std::uint32_t size,
                          std::uint32_t col, std::uint32_t row) {
    const float f = 1.0f / static_cast<float>(grid);
    // 半像素内缩（**关键**）：格子之间没有间隙，UV 正好落在格边界时，
    // 最近邻/线性过滤会取到**隔壁格**的纹素 —— 隔壁若是 default.png（品红+黑的
    // 缺失纹理兜底），方块边缘就会冒出一条条短紫线。内缩半像素让采样点始终在格内。
    const float inset = (size > 0) ? (0.5f / static_cast<float>(size)) : 0.0f;
    return { static_cast<float>(col) * f + inset, static_cast<float>(row) * f + inset,
             static_cast<float>(col + 1) * f - inset, static_cast<float>(row + 1) * f - inset };
}

glm::vec4 TextureAtlas::slotUV(AtlasSlot s) const {
    return atlasSlotUVPure(grid_, size_, s.col, s.row);
}

} // namespace engine::assets
