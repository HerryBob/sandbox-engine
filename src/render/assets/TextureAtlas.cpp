#include "TextureAtlas.hpp"
#include "core/Log.hpp"
#include "registry/BlockRegistry.hpp"
#include "render/assets/ModelRegistry.hpp"
#include "resource/ResourceIndex.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>
#include <cstring>
#include <stb_image.h>
#include <rapidjson/document.h>

namespace engine::assets {

bool TextureAtlas::build() {
    // ① 声明文件 assets/atlases/blocks.json 的 textures 数组：有序，逐格铺
    //    第 0 格恒为 default.png，声明里不用写（写了也跳过）
    std::vector<std::string> declared;
    {
        std::filesystem::path declPath;
        if (resource::g_resourceIndex.find(resource::ResourceKind::Assets,
                                           "atlases/blocks.json", declPath)) {
            std::ifstream in(declPath);
            if (!in) {
                LOG_WARN("Atlas", "图集声明打开失败：atlases/blocks.json");
            } else {
                const std::string text{std::istreambuf_iterator<char>(in),
                                       std::istreambuf_iterator<char>()};
                rapidjson::Document doc;
                doc.Parse(text.c_str());
                if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("textures") ||
                    !doc["textures"].IsArray()) {
                    LOG_WARN("Atlas", "图集声明解析失败或缺 textures 数组：atlases/blocks.json");
                } else {
                    std::set<std::string> uniq;
                    for (const auto& v : doc["textures"].GetArray()) {
                        if (!v.IsString()) continue;
                        const std::string name = v.GetString();
                        if (name.empty() || name == "default") continue;
                        if (uniq.insert(name).second) declared.push_back(name);
                    }
                }
            }
        } else {
            LOG_WARN("Atlas", "图集声明 atlases/blocks.json 缺失 → 回退按注册表收集");
        }
    }

    // 方块引用了别的模组提供的贴图：该模组没带全资源，或两个模组用了同一个纹理名。
    // 引擎（owner 为空串）或引擎自带的贴图不报 —— 公共基础设施。
    const auto warnCrossOwner = [](std::string_view texName, const std::string& blockOwner) {
        if (blockOwner.empty()) return;
        const std::string texOwner =
            resource::g_resourceIndex.ownerOf(resource::ResourceKind::Assets,
                                              "textures/block/" + std::string(texName) + ".png");
        if (texOwner.empty() || texOwner == "engine" || texOwner == blockOwner) return;
        LOG_WARN("Atlas", "贴图跨模组引用：" + std::string(texName) + " 由 " + texOwner +
                          " 提供，方块（" + blockOwner + "）正在使用");
    };

    // ② 注册表实际引用到的纹理名：既是声明缺失时的回退来源，也用于检查「引用了却没声明」
    std::vector<std::string> used;
    {
        std::set<std::string> uniq;
        for (const auto& [id, info] : engine::registry::g_blockRegistry) {
            if (info.texture.empty()) continue;
            if (info.texture == "default") continue;              // 第 0 格固定给它
            if (!uniq.insert(info.texture).second) continue;      // 去重：同名只填一次
            warnCrossOwner(info.texture, engine::registry::g_blockRegistry.ownerOf(id));
            used.push_back(info.texture);
        }
        for (const auto& [modelName, entry] : engine::assets::g_modelRegistry) {
            (void)modelName;
            for (const auto& [var, tex] : entry.value.textures) {
                (void)var;
                if (tex.empty() || tex == "default") continue;    // 第 0 格固定给它
                if (!uniq.insert(tex).second) continue;
                used.push_back(tex);
            }
        }
    }

    // ③ 没有声明 → 用注册表收集结果（与改造前行为一致）
    if (declared.empty()) declared = used;

    // ④ 每边格数：能放下「default + 声明条目」的最小方阵
    const std::uint32_t need = 1 + static_cast<std::uint32_t>(declared.size());
    grid_ = 1;
    while (grid_ * grid_ < need) ++grid_;
    size_ = kAtlasTile * grid_;
    pixels_.assign(static_cast<std::size_t>(size_) * size_ * kAtlasChannels, 0u);

    // ⑤ 第 0 格 = default.png（缺失纹理兜底，内容来自文件而非程序生成）
    if (!addFromFile("default", AtlasSlot{0, 0}))
        LOG_WARN("Atlas", "兜底纹理 default.png 缺失 → 第 0 格透明黑");

    // ⑥ 按声明顺序铺格（第 0 格已被 default 占用，从第 1 格起）
    std::uint32_t slotIndex = 1;
    std::uint32_t loaded = 0;
    std::uint32_t failed = 0;
    for (const std::string& name : declared) {
        if (slotIndex >= grid_ * grid_) break;                    // 容量已满
        const AtlasSlot slot{
            static_cast<std::uint8_t>(slotIndex / grid_),
            static_cast<std::uint8_t>(slotIndex % grid_)};
        // 失败原因由 addFromFile 打（含提供者），这里只计数
        if (addFromFile(name, slot)) {
            slots_.emplace(name, slot);
            ++loaded;
        } else {
            ++failed;
        }
        ++slotIndex;
    }

    // ⑦ 注册表引用了但声明没列 → 显示兜底格，汇总一条告警
    {
        std::string missing;
        for (const std::string& name : used) {
            if (slots_.find(name) != slots_.end()) continue;
            if (!missing.empty()) missing += ",";
            missing += name;
        }
        if (!missing.empty())
            LOG_WARN("Atlas", "以下纹理被引用但不在图集声明中，显示兜底格：" + missing);
    }

    LOG_INFO("Atlas", "图集 " + std::to_string(size_) + "²（" + std::to_string(grid_) + "×" +
                      std::to_string(grid_) + " 格）：声明 " + std::to_string(declared.size()) +
                      " 张，成功 " + std::to_string(loaded) + " 张，失败 " +
                      std::to_string(failed) + " 张，占用 " + std::to_string(slotIndex) + "/" +
                      std::to_string(grid_ * grid_) + " 格");
    return !pixels_.empty();
}

bool TextureAtlas::addFromFile(std::string_view name, AtlasSlot slot) {
    // 走资源索引：引擎与各模组提供的 textures/block/*.png 合并在同一张表里，引擎优先
    const std::string rel = "textures/block/" + std::string(name) + ".png";
    std::filesystem::path path;
    if (!resource::g_resourceIndex.find(resource::ResourceKind::Assets, rel, path)) {
        LOG_WARN("Atlas", "纹理未找到：" + rel + "（没有任何资源根提供）→ 该格透明黑");
        return false;
    }
    const std::string owner =
        resource::g_resourceIndex.ownerOf(resource::ResourceKind::Assets, rel);

    int w = 0, h = 0, ch = 0;
    const std::string pathStr = path.string();
    stbi_uc* src = stbi_load(pathStr.c_str(), &w, &h, &ch, STBI_rgb_alpha);
    if (!src) {
        LOG_WARN("Atlas", "纹理解码失败：" + rel + "（由 " + owner + " 提供）→ 该格透明黑");
        return false;
    }

    // 直拷模式：引擎不做任何缩放——源图尺寸必须等于格子尺寸（16²），原样进格。
    // 未来升像素：只需改 kAtlasTile（如 32）且源图同步 32²，此处零改动。
    if (w != static_cast<int>(kAtlasTile) || h != static_cast<int>(kAtlasTile)) {
        LOG_WARN("Atlas", "纹理尺寸不符：" + rel + "（由 " + owner + " 提供）为 " +
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

glm::vec4 TextureAtlas::slotUV(AtlasSlot s) const {
    const float f = 1.0f / static_cast<float>(grid_);
    return { s.col * f, s.row * f, (s.col + 1) * f, (s.row + 1) * f };
}

} // namespace engine::assets
