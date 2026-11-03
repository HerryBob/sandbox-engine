#include "HudTextBuilder.hpp"

namespace engine::render::hud {

void HudTextBuilder::addQuad(float x, float y, float w, float h,
                             const glm::vec4& uv, const glm::vec4& color) {
    // uv 是 {u0, v0, u1, v1}；四个角按 左上→右上→右下→左下 排列
    const glm::vec2 p0(x, y);
    const glm::vec2 p1(x + w, y);
    const glm::vec2 p2(x + w, y + h);
    const glm::vec2 p3(x, y + h);
    const glm::vec2 t0(uv.x, uv.y);
    const glm::vec2 t1(uv.z, uv.y);
    const glm::vec2 t2(uv.z, uv.w);
    const glm::vec2 t3(uv.x, uv.w);

    // 展开成 6 个顶点（两个三角形）而不是 4 顶点 + index buffer：
    // HUD 顶点量小（满屏字符也就几千个），换掉一条索引缓冲、少一次 drawIndexed 绑定。
    verts_.push_back(HudVertex{p0, t0, color});
    verts_.push_back(HudVertex{p1, t1, color});
    verts_.push_back(HudVertex{p2, t2, color});
    verts_.push_back(HudVertex{p0, t0, color});
    verts_.push_back(HudVertex{p2, t2, color});
    verts_.push_back(HudVertex{p3, t3, color});
}

void HudTextBuilder::addPanel(float x, float y, float w, float h, const glm::vec4& color) {
    addQuad(x, y, w, h, whiteUV(), color);
}

float HudTextBuilder::addText(float x, float y, const std::string& text,
                              const glm::vec4& color, float scale) {
    const float advance = static_cast<float>(kGlyphW) * scale;
    const float glyphH = static_cast<float>(kGlyphH) * scale;

    float penX = x;
    for (const char c : text) {
        const int code = static_cast<int>(static_cast<unsigned char>(c));
        glm::vec4 uv{};
        if (code >= kFirstGlyph && code <= kLastGlyph) {
            charUV(code, uv);
            // 空格只推进不生成四边形（图集里它本来就是空白）
            if (code != ' ') {
                addQuad(penX, y, static_cast<float>(kGlyphW) * scale, glyphH, uv, color);
            }
        }
        // 非 ASCII（比如误传中文）按一个字宽跳过，保证其后字符不错位
        penX += advance;
    }
    return penX - x;
}

float HudTextBuilder::textWidth(const std::string& text, float scale) const {
    // 位图字体等宽：宽度 = 字符数 × 字宽（与 addText 的推进完全一致）
    return static_cast<float>(text.size()) * static_cast<float>(kGlyphW) * scale;
}

} // namespace engine::render::hud
