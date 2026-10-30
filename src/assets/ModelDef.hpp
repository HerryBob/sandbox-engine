#pragma once
// 方块模型定义（MC 兼容子集）：elements（盒子）+ textures（纹理变量表）。
// 语义：数据驱动网格生成——meshing 读模型生成顶点，替代硬编码立方体。
// 坐标：元素 from/to 用 16 单位（一格 = 16×16×16，与 MC 模型空间同构）。
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

namespace engine::assets {

// 面：一个立方体元素有 6 面（索引 0..5 = +X -X +Y -Y +Z -Z，与面掩码位一致）
struct ModelFace {
    std::string texture;        // 纹理名（已展开：JSON 的 "#top" → textures 表 → 真实名）
    std::uint8_t cullFace = 0;  // 0=永不剔除；1..6 = +X -X +Y -Y +Z -Z
                                // （该方向邻块不透明则剔此面）
};

// 元素：一个盒子（from/to 16 单位坐标）
struct ModelElement {
    glm::vec3 from{0.0f, 0.0f, 0.0f};
    glm::vec3 to{16.0f, 16.0f, 16.0f};
    std::array<ModelFace, 6> faces;   // +X -X +Y -Y +Z -Z
};

// 模型：元素列表 + 纹理变量表（"#side" → "log_side"）
struct ModelDef {
    std::vector<ModelElement> elements;
    std::unordered_map<std::string, std::string> textures;
};

} // namespace engine::assets
