#version 450

// 相机矩阵：CPU 已预乘成 proj * view（model 恒为单位阵，折进去了）。
layout(binding = 0, std140) uniform UniformBufferObject
{
    mat4 viewProj;
} ubo;

// 逐 draw 的模型矩阵（刚体 / 骨骼 / 静态趟的单位阵），走 push constant。
layout(push_constant) uniform PushBody
{
    mat4 bodyModel;
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
    // viewProj * bodyModel 两者都是 uniform ⇒ 编译器按 draw 提到遍历外，
    // 每个顶点只剩一次 mat4 × vec4。
    gl_Position = ubo.viewProj * pc.bodyModel * vec4(inPosition, 1.0);
}
