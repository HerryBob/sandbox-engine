#pragma once
// 实体模型（bbmodel 的引擎形态）：骨骼树 + 每骨骼的立方体 + 贴图引用。
// 与 ModelDef（方块模型）并列——方块是 elements 平铺，实体是 bones 树。
// 坐标：16 单位 = 1 格；加载时已归一化到「脚底中心为原点、y 向上」。
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace engine::assets {

// 立方体单面 UV：bbmodel 导出的是 [u0,v0,u1,v1]，v 向下（图像坐标）。
struct EntityFaceUV {
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
};

// 一个立方体：16 单位空间的 from/to，面序与 ModelDef 一致（+X -X +Y -Y +Z -Z）。
struct EntityCube {
    glm::vec3    from{0.0f};
    glm::vec3    to{0.0f};
    EntityFaceUV faces[6];
    int          textureIndex = 0;   // 指向 EntityModel::texturePaths
};

// 一根骨骼：名字 + 父索引 + 枢轴 + 挂在自己身上的立方体。
// pivot 是旋转中心（16 单位空间，相对模型原点）；父子关系决定变换累乘顺序。
struct EntityBone {
    std::string              name;
    int                      parent = -1;      // -1 = 根
    glm::vec3                pivot{0.0f};
    std::vector<EntityCube>  cubes;
};

// 一个完整实体模型。
struct EntityModel {
    int                      texWidth = 64;
    int                      texHeight = 64;
    std::vector<EntityBone>  bones;
    std::vector<std::string> texturePaths;   // bbmodel textures[].relative_path
};

} // namespace engine::assets
