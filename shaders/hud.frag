#version 450

// HUD 片元着色器：位图字体采样。
//   图集是黑底白字（单色栅格化），R 通道即"覆盖率"：
//     · 字形像素      → R = 1 → 该处不透明度 = fragColor.a
//     · 空白像素      → R = 0 → 完全透明
//   背板四边形指向图集第 0 格（纯白），覆盖率恒 1，因此同一管线既能画字也能画半透明底。

layout(binding = 0) uniform sampler2D fontTex;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;

layout(location = 0) out vec4 outColor;

void main()
{
    float coverage = texture(fontTex, fragUV).r;
    outColor = vec4(fragColor.rgb, fragColor.a * coverage);
}
