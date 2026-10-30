#version 450

layout(binding = 1) uniform sampler2D texSampler;

layout(location = 0) out vec4 outColor;
layout(location = 1) in vec2 fragUV;
layout(location = 0) in vec3 fragColor;

void main()
{
    vec4 c = texture(texSampler, fragUV);
    if (c.a < 0.5) discard;
    outColor = c;
}
