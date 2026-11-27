#pragma once
#include <cstdint>

#include "shared/ui/UiCmd.hpp"

namespace engine::business::ui {

// ============================================================================
// 每张 UI 图的布局表（"整张图 + 程序高亮"方案的地基）
// ============================================================================
//
// 决策（2026-11-27 用户定）：
//   · UI 美术**就是整张图**（一张 PNG 一个界面），替换时只换文件 → 便于模组化；
//   · 高亮/命中/槽位**由程序画**，所以每张图要一份"槽位网格在哪"的布局表；
//   · 布局表按图单独写（"单独为每张图专门做处理"）—— 美术换图时只改这张表的数字。
//
// 为什么布局表放在业务侧而不是渲染侧：
//   渲染线程只消费 UiCmd 指令（它不知道"背包"是什么）。
//   槽位布局属于"界面语义"，归 UI 内容层；命中测试与绘制共用同一份数字，不会错位。
//
// 坐标全部是**素材像素**（图内），屏幕位置由调用方按缩放/居中换算。

// 网格：cols×rows 个等距槽位
struct SlotGrid {
    float x = 0.0f, y = 0.0f;      // 第一格左上角（素材像素）
    float pitchX = 17.0f;          // 列步进
    float pitchY = 17.0f;          // 行步进
    float slotW = 15.0f, slotH = 15.0f;
    int   cols = 1, rows = 1;

    bool valid() const { return cols > 0 && rows > 0 && slotW > 0.0f && slotH > 0.0f; }
    int  count() const { return cols * rows; }
    // 第 i 格的矩形（素材像素）
    engine::shared::ui::Rect cell(int i) const {
        using engine::shared::ui::Rect;
        if (!valid() || i < 0 || i >= count()) return Rect{};
        const int c = i % cols;
        const int r = i / cols;
        return Rect{ x + static_cast<float>(c) * pitchX,
                     y + static_cast<float>(r) * pitchY, slotW, slotH };
    }
    // 命中测试：素材像素 (px,py) 落在第几格？-1 = 不在任何格内
    int hit(float px, float py) const {
        if (!valid()) return -1;
        const float lx = px - x;
        const float ly = py - y;
        if (lx < 0.0f || ly < 0.0f) return -1;
        const int c = static_cast<int>(lx / pitchX);
        const int r = static_cast<int>(ly / pitchY);
        if (c >= cols || r >= rows) return -1;
        // 落在"格间缝隙"里不算命中（格子只有 slotW×slotH 大）
        if (lx - static_cast<float>(c) * pitchX > slotW) return -1;
        if (ly - static_cast<float>(r) * pitchY > slotH) return -1;
        return r * cols + c;
    }
};

// 一个界面的完整布局（整图 + 若干网格/单格 + 整图内容区）
struct UiLayout {
    const char*  imagePath = nullptr;   // 素材路径（渲染端上传用）
    std::uint8_t texture = 0;           // 纹理槽（业务与渲染约定的编号）
    int   imageW = 256, imageH = 256;   // 素材尺寸（UV 归一化用）
    engine::shared::ui::Rect content{}; // 内容区（素材像素；整图绘制时只画这块）
    SlotGrid mainGrid;                  // 主网格（背包 9×6 / 工作台 6×6）
    SlotGrid sideGrid;                  // 副网格（工作台 3×3；其它图 rows=0 表示无）
    SlotGrid bottomBar;                 // 底部快捷栏条（9 格）
    engine::shared::ui::Rect singleSlot{};  // 图里的单个格（如左下角）
    engine::shared::ui::Rect resultSlot{};  // 输出格（工作台）
};

// ---- 实测布局（2026-11-27，用 Python 逐像素量的；换美术只改这里）----

// 快捷栏：9 连格条 x37..218 y117..138；单格 x3..24
inline const UiLayout& layoutHotbar() {
    static const UiLayout L = [] {
        UiLayout l;
        l.imagePath = "assets/textures/ui/hotbar.png";
        l.texture = 1;
        l.imageW = l.imageH = 256;
        l.content = engine::shared::ui::Rect{ 3.0f, 117.0f, 216.0f, 22.0f };
        // 条内 9 格：pitch 20 / 槽 16（起点比条左缘内缩 4）
        l.mainGrid = SlotGrid{ 41.0f, 119.0f, 20.0f, 20.0f, 16.0f, 16.0f, 9, 1 };
        l.singleSlot = engine::shared::ui::Rect{ 5.0f, 119.0f, 18.0f, 18.0f };
        return l;
    }();
    return L;
}

// 背包：主网格 9×6（起点 53,67，pitch 17，槽 15）+ 底部 9 格 + 左下单格
inline const UiLayout& layoutBackpack() {
    static const UiLayout L = [] {
        UiLayout l;
        l.imagePath = "assets/textures/ui/backpack.png";
        l.texture = 2;
        l.imageW = l.imageH = 256;
        l.content = engine::shared::ui::Rect{ 25.0f, 61.0f, 184.0f, 133.0f };
        l.mainGrid = SlotGrid{ 53.0f, 67.0f, 17.0f, 17.0f, 15.0f, 15.0f, 9, 6 };
        l.bottomBar = SlotGrid{ 53.0f, 180.0f, 17.0f, 17.0f, 15.0f, 15.0f, 9, 1 };
        l.singleSlot = engine::shared::ui::Rect{ 19.0f, 180.0f, 15.0f, 15.0f };
        return l;
    }();
    return L;
}

// 工作台：主网格 6×6（15,9，pitchX 17 / pitchY 18）+ 右上 3×3（140,9，pitch 17）
//         + 输出格（226,26）+ 底部 9 格 + 左下单格
inline const UiLayout& layoutCrafting() {
    static const UiLayout L = [] {
        UiLayout l;
        l.imagePath = "assets/textures/ui/crafting.png";
        l.texture = 3;
        l.imageW = l.imageH = 256;
        l.content = engine::shared::ui::Rect{ 0.0f, 0.0f, 256.0f, 198.0f };
        l.mainGrid = SlotGrid{ 15.0f, 9.0f, 17.0f, 18.0f, 15.0f, 15.0f, 6, 6 };
        l.sideGrid = SlotGrid{ 140.0f, 9.0f, 17.0f, 17.0f, 15.0f, 15.0f, 3, 3 };
        l.bottomBar = SlotGrid{ 53.0f, 180.0f, 17.0f, 17.0f, 15.0f, 15.0f, 9, 1 };
        l.singleSlot = engine::shared::ui::Rect{ 19.0f, 180.0f, 15.0f, 15.0f };
        l.resultSlot = engine::shared::ui::Rect{ 226.0f, 26.0f, 15.0f, 15.0f };
        return l;
    }();
    return L;
}

// ---- 由布局表生成绘制指令（整图 + 程序高亮）----
// screen = 屏幕尺寸；scale = 素材像素放大倍数（整数保像素风锐利）；
// origin = 整图左上角在屏幕上的位置（调用方决定居中/靠底）；
// hoverSlot / selectedSlot 用主网格索引，-1 表示无。
inline void buildImageUi(const UiLayout& L, float screenW, float screenH, float scale,
                         engine::shared::ui::Rect origin, int hoverSlot, int selectedSlot,
                         std::vector<engine::shared::ui::UiCmd>& out) {
    using namespace engine::shared::ui;
    if (L.imagePath == nullptr || scale <= 0.0f) return;

    const Rect uv = uvFromPixels(L.content.x, L.content.y, L.content.w, L.content.h,
                                 L.imageW, L.imageH);
    const Rect dst{ origin.x + L.content.x * scale, origin.y + L.content.y * scale,
                    L.content.w * scale, L.content.h * scale };
    pushSprite(out, dst, uv, L.texture, 1.0f, 1.0f, 1.0f, 1.0f, /*layer=*/10);

    // 高亮：程序画在槽位上（美术图不必画选中态）
    auto slotToDst = [&](const SlotGrid& g, int idx) {
        const Rect c = g.cell(idx);
        return Rect{ origin.x + c.x * scale, origin.y + c.y * scale, c.w * scale, c.h * scale };
    };
    if (selectedSlot >= 0) {
        const Rect r = slotToDst(L.mainGrid, selectedSlot);
        pushQuad(out, r, 1.0f, 1.0f, 1.0f, 0.30f, /*layer=*/11);
    }
    if (hoverSlot >= 0) {
        const Rect r = slotToDst(L.mainGrid, hoverSlot);
        pushQuad(out, r, 1.0f, 0.92f, 0.55f, 0.30f, /*layer=*/12);
    }
}

} // namespace engine::business::ui
