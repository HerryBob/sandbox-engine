#version 450

// 逐 draw 的两个矩阵都走 push constant（共 128 字节 = 2 个 mat4，规范保证的下限）：
//   bodyModel：刚体 / 骨骼 / 静态趟的单位阵
//   viewProj ：CPU 预乘的 proj * view（model 恒为单位阵，已折进去）
//
// 为什么不用 UBO：相机矩阵以前放在一块共享 UBO 里，而渲染有 2 个 frame-in-flight，
// CPU 写第 N 帧的矩阵时会覆盖第 N-1 帧仍在读取的内容 → 偶发一帧相机矩阵错乱
// （表现为"一片地形突然变天空色，只闪一帧"）。push constant 跟着命令缓冲走，无共享。
layout(push_constant) uniform PushBody
{
    mat4 bodyModel;
    mat4 viewProj;
} pc;

layout(location = 0) out vec3 fragColor;
layout(location = 1) in vec3 inColor;
layout(location = 1) out vec2 fragUV;
layout(location = 2) in vec2 inUV;
layout(location = 0) in vec3 inPosition;

void main()
{
    fragColor = inColor;
    fragUV = inUV;
    // 每个顶点只剩一次 mat4 × vec4（两个矩阵都在 push constant 里，逐 draw 常量）。
    gl_Position = pc.viewProj * pc.bodyModel * vec4(inPosition, 1.0);
}
