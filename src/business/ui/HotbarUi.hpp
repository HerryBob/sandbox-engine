#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "shared/ui/UiCmd.hpp"

namespace engine::business::ui {

// ============================================================================
// 快捷栏（内容层：只产出绘制指令，不认识 Vulkan）
// ============================================================================
//
// 这一层的定位：**UI 的作者**。它决定"快捷栏长什么样、第几格选中、显示什么数字"，
// 但只知道 shared/ui/UiCmd.hpp 里的指令类型 —— 所以：
//   · 可以完全脱离渲染线程/GPU 单测（见 core/SelfTest.cpp 的 uiLayout）
//   · 将来搬进 DLL 模组时，模组只需 include 这个头 + 往指令表里写
//
// 素材：assets/textures/ui/hotbar.png（256×256，实测内容）
//   · 单格样板：x 3..24,  y 117..138   (22×22)
//   · 9 连格条：x 37..218, y 117..138  (182×22，9 格步进 ≈20.2)
// 素材按 **NEAREST + 整数缩放** 绘制，保持像素风锐利。

constexpr int kHotbarSlots = 9;

// 素材里的两个区域（像素）
constexpr float kSheetSlotX = 3.0f,  kSheetSlotY = 117.0f, kSheetSlotW = 22.0f, kSheetSlotH = 22.0f;
constexpr float kSheetStripX = 37.0f, kSheetStripY = 117.0f, kSheetStripW = 182.0f, kSheetStripH = 22.0f;
constexpr int   kSheetSize = 256;

// 纹理槽分配（渲染端按此上传；业务只引用编号）
constexpr std::uint8_t kTexHotbar = 1;   // assets/textures/ui/hotbar.png

// 快捷栏状态（业务权威；将来物品栏/合成台共用同一套槽位结构）
struct HotbarState {
    bool visible = true;
    int  selected = 0;                       // 0..kHotbarSlots-1
    std::array<std::uint16_t, kHotbarSlots> itemId{};   // 0 = 空
    std::array<int, kHotbarSlots> itemCount{};
};

struct HotbarLayout {
    float scale = 3.0f;        // 像素放大倍数（整数保锐利）
    float marginBottom = 14.0f;
};

// 快捷栏条在屏幕上的矩形（供命中测试/高亮用）
inline engine::shared::ui::Rect hotbarRect(float screenW, float screenH, const HotbarLayout& L) {
    using namespace engine::shared::ui;
    return bottomCenterRect(screenW, screenH, kSheetStripW * L.scale, kSheetStripH * L.scale,
                            L.marginBottom);
}

// 第 i 格在屏幕上的矩形（按条均分；用于选中高亮与将来图标定位）
inline engine::shared::ui::Rect hotbarSlotRect(float screenW, float screenH,
                                               const HotbarLayout& L, int i) {
    using namespace engine::shared::ui;
    const Rect strip = hotbarRect(screenW, screenH, L);
    const float pitch = strip.w / static_cast<float>(kHotbarSlots);
    return Rect{ strip.x + pitch * static_cast<float>(i), strip.y, pitch, strip.h };
}

// 生成一帧快捷栏指令（背景条 + 选中高亮 + 槽位数字）。
// 图标本身等美术给"物品图标图集"后再填（itemId 已留好位）。
inline void buildHotbar(const HotbarState& st, float screenW, float screenH,
                        const HotbarLayout& L, std::vector<engine::shared::ui::UiCmd>& out) {
    using namespace engine::shared::ui;
    if (!st.visible || screenW <= 0.0f || screenH <= 0.0f) return;

    const Rect strip = hotbarRect(screenW, screenH, L);

    // ① 底板：整条 9 连格（一次 sprite；NEAREST 放大保锐利）
    const Rect uvStrip = uvFromPixels(kSheetStripX, kSheetStripY, kSheetStripW, kSheetStripH,
                                      kSheetSize, kSheetSize);
    pushSprite(out, strip, uvStrip, kTexHotbar, 1.0f, 1.0f, 1.0f, 1.0f, /*layer=*/10);

    // ② 选中高亮：在选中格上叠一层半透明白（像素边缘不缩放，直接从格矩形算）
    const int sel = (st.selected < 0) ? 0 : (st.selected % kHotbarSlots);
    const Rect slot = hotbarSlotRect(screenW, screenH, L, sel);
    const float inset = 2.0f * L.scale;   // 往里收一点，别盖住格子描边
    pushQuad(out, Rect{ slot.x + inset, slot.y + inset,
                        slot.w - inset * 2.0f, slot.h - inset * 2.0f },
             1.0f, 1.0f, 1.0f, 0.28f, /*layer=*/11);

    // ③ 槽位数字（1..9），画在格子左上角内侧
    for (int i = 0; i < kHotbarSlots; ++i) {
        const Rect r = hotbarSlotRect(screenW, screenH, L, i);
        char label[2] = { static_cast<char>('1' + i), '\0' };
        pushText(out, label, r.x + 3.0f * L.scale, r.y + 2.0f * L.scale, L.scale,
                 0.10f, 0.12f, 0.16f, 0.85f, /*layer=*/12);
    }
}

} // namespace engine::business::ui
