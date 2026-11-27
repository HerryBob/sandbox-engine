#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace engine::shared::ui {

// ============================================================================
// UI 绘制指令（与 Vulkan 完全无关）
// ============================================================================
//
// 为什么要这一层（模组化的前提）：
//   ① 「UI 长什么样、摆在哪」由**业务/模组**决定，它不该碰 Vulkan、也不该跑在渲染线程；
//   ② 渲染线程只做一件事：把指令表变成顶点、批量提交。它不认识"快捷栏""背包"；
//   ③ 将来的 DLL 模组只需产出这份指令表（POD + 固定大小），不需要链接任何渲染代码。
//
// 数据流：
//    业务/模组  ──填指令──▶  SPSC 队列（RenderBridge）  ──▶  渲染线程 UiRenderer
//
// 一帧的边界用 FrameBegin / FrameEnd 标记：渲染端先丢半帧，再整帧应用，
// 所以即使某帧渲染慢了，也不会把两帧的指令混在一起（UI 是"状态"，不是事件）。

struct Rect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

enum class CmdType : std::uint8_t {
    FrameBegin = 0,   // 本帧指令开始（渲染端清空未完成的上帧）
    FrameEnd,         // 本帧结束（可以提交了）
    Quad,             // 纯色矩形（底板、高亮、分割线）
    Sprite,           // 纹理矩形（图标、九宫格面板、格子图）
    Text,             // 文本（由渲染端用位图字体展开成多个 Quad）
};

// 纹理槽：0 固定是"字体图集"（Text 必须用它）；1.. 是 UI 图集（每张图一个槽）。
// 槽位由渲染端在上传时分配，模组只按名字/编号引用，不碰 VkDescriptorSet。
constexpr std::uint8_t kTextureFont = 0;

// 指令长度固定（含 16 字节文本），便于 SPSC 队列按值拷贝。
// 快捷栏只需要 "1".."9"；将来物品数量显示最多 3~4 位，16 字节足够。
constexpr int kTextCapacity = 16;

struct UiCmd {
    CmdType  type = CmdType::Quad;
    std::uint8_t  texture = 0;      // Sprite/Text 用
    std::uint16_t layer = 0;        // 绘制顺序（大的画在上面）
    Rect     dst{};                 // 目标矩形（屏幕像素，左上原点、y 向下）
    Rect     uv{};                  // 纹理坐标（0..1；Quad 忽略）
    Rect     clip{};                // 裁剪矩形（屏幕像素）；w <= 0 表示不裁剪
    float    color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    char     text[kTextCapacity] = {};

    void setText(const char* s) {
        std::memset(text, 0, sizeof(text));
        if (s == nullptr) return;
        std::strncpy(text, s, kTextCapacity - 1);
    }
};

// ---- 小工具（纯函数，可单独测）----

// 像素矩形 → 归一化 UV
inline Rect uvFromPixels(float x, float y, float w, float h, int texW, int texH) {
    if (texW <= 0 || texH <= 0) return Rect{};
    const float fw = static_cast<float>(texW);
    const float fh = static_cast<float>(texH);
    return Rect{ x / fw, y / fh, w / fw, h / fh };
}

// 屏幕底部居中的矩形（快捷栏用）：marginBottom 是离屏幕底边的距离
inline Rect bottomCenterRect(float screenW, float screenH, float w, float h, float marginBottom) {
    return Rect{ (screenW - w) * 0.5f, screenH - marginBottom - h, w, h };
}

// 水平居中的矩形（任意 y）
inline Rect centerXRect(float screenW, float y, float w, float h) {
    return Rect{ (screenW - w) * 0.5f, y, w, h };
}

// ---- 九宫格：把一个纹理矩形拆成 9 块，边角不拉伸、四边单向拉伸 ----
// border 是像素边框宽度（左右上下相同）；返回 3×3 的 dst/uv 对。
struct Slice9 {
    Rect dst[9];
    Rect uv[9];
};

inline Slice9 makeNineSlice(Rect dst, Rect uv, float border) {
    Slice9 out{};
    // 切分尺寸（避免 2*border 超过目标尺寸导致负宽）
    const float bx = border, by = border;
    const float dw = dst.w - 2.0f * bx, dh = dst.h - 2.0f * by;
    const float uw = uv.w, uh = uv.h;
    const float ubx = (uv.w > 0.0f) ? border * (uv.w / (dst.w > 0.0f ? dst.w : 1.0f)) : 0.0f;
    const float uby = (uv.h > 0.0f) ? border * (uv.h / (dst.h > 0.0f ? dst.h : 1.0f)) : 0.0f;
    const float dx[3] = { dst.x, dst.x + bx, dst.x + bx + dw };
    const float dy[3] = { dst.y, dst.y + by, dst.y + by + dh };
    const float wx[3] = { bx, dw, bx };
    const float wy[3] = { by, dh, by };
    const float ux[3] = { uv.x, uv.x + ubx, uv.x + uw - ubx };
    const float uy[3] = { uv.y, uv.y + uby, uv.y + uh - uby };
    const float uwx[3] = { ubx, uw - 2.0f * ubx, ubx };
    const float uwy[3] = { uby, uh - 2.0f * uby, uby };
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const int i = r * 3 + c;
            out.dst[i] = Rect{ dx[c], dy[r], wx[c], wy[r] };
            out.uv[i] = Rect{ ux[c], uy[r], uwx[c], uwy[r] };
        }
    }
    return out;
}

// 追加一条纯色矩形
inline void pushQuad(std::vector<UiCmd>& out, Rect dst, float r, float g, float b, float a,
                     std::uint16_t layer = 0, Rect clip = Rect{}) {
    UiCmd c;
    c.type = CmdType::Quad;
    c.layer = layer;
    c.dst = dst;
    c.clip = clip;
    c.color[0] = r; c.color[1] = g; c.color[2] = b; c.color[3] = a;
    out.push_back(c);
}

// 追加一条纹理矩形
inline void pushSprite(std::vector<UiCmd>& out, Rect dst, Rect uv, std::uint8_t texture,
                       float r = 1.0f, float g = 1.0f, float b = 1.0f, float a = 1.0f,
                       std::uint16_t layer = 0, Rect clip = Rect{}) {
    UiCmd c;
    c.type = CmdType::Sprite;
    c.texture = texture;
    c.layer = layer;
    c.dst = dst;
    c.uv = uv;
    c.clip = clip;
    c.color[0] = r; c.color[1] = g; c.color[2] = b; c.color[3] = a;
    out.push_back(c);
}

// 追加一段文本（渲染端用位图字体展开；scale = 每个字体像素放大倍数）
inline void pushText(std::vector<UiCmd>& out, const char* s, float x, float y, float scale,
                     float r = 1.0f, float g = 1.0f, float b = 1.0f, float a = 1.0f,
                     std::uint16_t layer = 0, Rect clip = Rect{}) {
    UiCmd c;
    c.type = CmdType::Text;
    c.texture = kTextureFont;
    c.layer = layer;
    c.dst = Rect{ x, y, scale, 0.0f };   // h 不用（字高由字体表决定），scale 放 h 里更直观
    c.dst.h = scale;
    c.clip = clip;
    c.color[0] = r; c.color[1] = g; c.color[2] = b; c.color[3] = a;
    c.setText(s);
    out.push_back(c);
}

} // namespace engine::shared::ui
