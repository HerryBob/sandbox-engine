#pragma once
// HUD 位图字体布局（与 assets/textures/font/ascii.png 一一对应）。
// 图集由 tools/gen_font_atlas.ps1 离线生成，产物入库；运行时引擎不依赖系统字体。
#include <glm/glm.hpp>

namespace engine::render::hud {

// ---- 图集几何（改这些常量必须同步重跑 gen_font_atlas.ps1）----
constexpr int kGlyphW = 10;    // 单字符格宽（像素）
constexpr int kGlyphH = 16;    // 单字符格高（像素）
constexpr int kAtlasCols = 16; // 图集列数
constexpr int kAtlasRows = 6;  // 图集行数
constexpr int kAtlasW = kGlyphW * kAtlasCols; // 160
constexpr int kAtlasH = kGlyphH * kAtlasRows; // 96

// 图集覆盖的 ASCII 区间：32(空格) .. 126(~)，共 95 个可打印字符
constexpr int kFirstGlyph = 32;
constexpr int kLastGlyph = 126;

// 格序号 → UV 矩形 {u0, v0, u1, v1}（归一化 0~1）。
// 注意 v 方向：图集按"左上角为原点"存像素，而 UV 的 v=0 在纹理顶部，
// 所以第 row 行的 v0 = row*kGlyphH / kAtlasH —— 与像素坐标同向，无需翻转。
inline glm::vec4 glyphSlotUV(int slot) {
    const int col = slot % kAtlasCols;
    const int row = slot / kAtlasCols;
    const float u0 = static_cast<float>(col * kGlyphW) / static_cast<float>(kAtlasW);
    const float v0 = static_cast<float>(row * kGlyphH) / static_cast<float>(kAtlasH);
    const float u1 = static_cast<float>((col + 1) * kGlyphW) / static_cast<float>(kAtlasW);
    const float v1 = static_cast<float>((row + 1) * kGlyphH) / static_cast<float>(kAtlasH);
    return glm::vec4(u0, v0, u1, v1);
}

// 第 0 格恒为纯白块：HUD 背板四边形采样它，覆盖率 1 → 纯色半透明矩形。
inline glm::vec4 whiteUV() { return glyphSlotUV(0); }

// 字符 → UV。超出 ASCII 可打印范围返回 false（调用方自行跳过或替换）。
inline bool charUV(int code, glm::vec4& out) {
    if (code < kFirstGlyph || code > kLastGlyph) return false;
    out = glyphSlotUV(code - kFirstGlyph + 1); // 格 0 是白块，字符从格 1 起
    return true;
}

} // namespace engine::render::hud
