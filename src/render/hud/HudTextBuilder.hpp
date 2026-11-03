#pragma once
// HUD 文本构建器：把字符串切成"每字符两个三角形"的顶点流（纯 CPU，不碰 Vulkan）。
#include "HudFont.hpp"

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace engine::render::hud {

// 单个 HUD 顶点（与 VulkanContext 里 HUD 管线的顶点布局一致）：
//   location 0 = pos（像素坐标，左上原点，y 向下）
//   location 1 = uv（字体图集，0~1）
//   location 2 = color（RGBA，线性）
struct HudVertex {
    glm::vec2 pos;
    glm::vec2 uv;
    glm::vec4 color;
};
static_assert(sizeof(HudVertex) == 32, "HudVertex 必须是紧凑的 32 字节（vec2+vec2+vec4）");

class HudTextBuilder {
public:
    // 行高（像素）= 字高 16 + 行间 2
    static constexpr float kLineHeight = static_cast<float>(kGlyphH + 2);

    void clear() { verts_.clear(); }
    const std::vector<HudVertex>& vertices() const { return verts_; }
    std::size_t vertexCount() const { return verts_.size(); }

    // 纯色矩形（采样第 0 格白块）：画 HUD 背板
    void addPanel(float x, float y, float w, float h, const glm::vec4& color);

    // 一行文本。x/y 为左上角像素坐标；返回这一行占用的宽度。
    // 只认 ASCII 32..126，其余字符按空格宽度跳过（不崩、不画乱码）。
    float addText(float x, float y, const std::string& text,
                  const glm::vec4& color, float scale = 1.0f);

    // 文本像素宽度（用于算背板尺寸；与 addText 的排布一致）
    float textWidth(const std::string& text, float scale = 1.0f) const;

private:
    void addQuad(float x, float y, float w, float h,
                 const glm::vec4& uv, const glm::vec4& color);

    std::vector<HudVertex> verts_;
};

} // namespace engine::render::hud
