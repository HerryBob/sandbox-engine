#pragma once
// 方块模型定义（MC 兼容子集）：elements（盒子）+ textures（纹理变量表）。
// 语义：数据驱动网格生成——meshing 读模型生成顶点，替代硬编码立方体。
// 坐标：元素 from/to 用 16 单位（一格 = 16×16×16，与 MC 模型空间同构）。
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

namespace engine::assets {

// 面：一个立方体元素有 6 面（索引 0..5 = +X -X +Y -Y +Z -Z，与面掩码位一致）
struct ModelFace {
    std::string texture;        // 纹理名：烘焙前是 "#变量" 原样，烘焙后展开为真实名
    std::uint8_t cullFace = 0;  // 0=永不剔除；1..6 = +X -X +Y -Y +Z -Z
                                // （该方向邻块不透明则剔此面）
};

// 元素旋转：单个盒子绕 origin 沿 axis 转 angle 度（只管这个盒子自己，不是整个方块的状态旋转）
struct ModelRotation {
    glm::vec3 origin{8.0f, 8.0f, 8.0f};   // 旋转原点（16 单位空间）
    char axis = 'y';                       // 'x' / 'y' / 'z'
    float angle = 0.0f;                    // 角度（度）
    bool rescale = false;                  // 旋转后缩放回原体积
};

// 16 单位空间里绕 rotation.origin 旋转一个点
inline glm::vec3 applyModelRotation(const glm::vec3& p, const ModelRotation& r) {
    if (r.angle == 0.0f) return p;
    const float rad = r.angle * 3.14159265358979323846f / 180.0f;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    const glm::vec3 d = p - r.origin;
    if (r.axis == 'x')      return r.origin + glm::vec3{d.x, c * d.y - s * d.z, s * d.y + c * d.z};
    else if (r.axis == 'z') return r.origin + glm::vec3{c * d.x - s * d.y, s * d.x + c * d.y, d.z};
    return r.origin + glm::vec3{c * d.x + s * d.z, d.y, -s * d.x + c * d.z};   // 'y'
}

// 元素：一个盒子（from/to 16 单位坐标）
struct ModelElement {
    glm::vec3 from{0.0f, 0.0f, 0.0f};
    glm::vec3 to{16.0f, 16.0f, 16.0f};
    std::array<ModelFace, 6> faces;   // +X -X +Y -Y +Z -Z
    ModelRotation rotation;           // angle=0 表示不旋转
};

// 模型：父模型 + 元素列表 + 纹理变量表（"#side" → "log_side"）
// 注册表里存的是烘焙后的最终形态；parent 仅保留原始记录供排查。
struct ModelDef {
    std::string parent;                                 // 父模型名（"block/cube_all"），空 = 无父
    std::vector<ModelElement> elements;
    std::unordered_map<std::string, std::string> textures;
};

} // namespace engine::assets
