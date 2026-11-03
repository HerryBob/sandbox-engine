#version 450

// HUD（游戏内调试浮层）顶点着色器：屏幕空间，像素坐标 → NDC。
//   · 顶点位置用像素表示，原点在左上角、y 轴向下（和写代码时的直觉一致）；
//   · 屏幕尺寸走 push constant（窗口 resize 后不需要重建管线）；
//   · 不涉及 MVP/UBO，HUD 完全独立于 3D 相机。

layout(push_constant) uniform HudPush {
    vec2 screenSize;   // (宽, 高) 像素
} pc;

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

void main()
{
    // 像素坐标 → NDC。注意 Vulkan 的 NDC 是 +y 指向屏幕下方（与 OpenGL 相反），
    // 而这里的像素坐标本来就是"左上原点、y 向下"，方向一致 → 直接线性映射，不要再翻一次。
    vec2 ndc = vec2(
        inPos.x / pc.screenSize.x * 2.0 - 1.0,
        inPos.y / pc.screenSize.y * 2.0 - 1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    fragUV = inUV;
    fragColor = inColor;
}
