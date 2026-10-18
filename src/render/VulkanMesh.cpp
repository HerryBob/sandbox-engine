#include "VulkanMesh.hpp"

namespace engine::render::mesh {

    // ---- 方块几何：24 顶点 + 36 索引（每面独立 4 顶点、独立颜色）----

    // 面定义表：法线 n + 主轴 u + 副轴 v（约束：u×v = n，右手系）
    // 角点由公式生成 → 叉积恒 = 4h²·n → 绕序数学上永远朝外
    static const glm::vec3 kFaceN[6] = {
        { 1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0,-1, 0}, {0, 0, 1}, {0, 0,-1}
    }; // 法线：+X  -X  +Y  -Y  +Z  -Z
    static const glm::vec3 kFaceU[6] = {
        {0,1,0}, {0,0,1}, {0,0,1}, {1,0,0}, {1,0,0}, {0,1,0}
    }; // 主轴
    static const glm::vec3 kFaceV[6] = {
        {0,0,1}, {0,1,0}, {1,0,0}, {0,0,1}, {0,1,0}, {1,0,0}
    }; // 副轴

    std::vector<Vertex> buildCubeVertices(const glm::vec3& offset) {
        const glm::vec2 faceUV[4] = {                 // 每面 4 角的纹理坐标
            {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}
        };
        const glm::vec3 c[6] = {
            {1.00f, 0.55f, 0.20f}, // +X 橙（木面）
            {0.55f, 0.35f, 0.15f}, // -X 深棕
            {0.75f, 0.60f, 0.35f}, // +Y 浅棕（顶）
            {0.45f, 0.30f, 0.15f}, // -Y 深棕（底）
            {0.85f, 0.50f, 0.25f}, // +Z 中棕
            {0.35f, 0.22f, 0.12f}, // -Z 最深棕
        };
        const float h = 0.5f; // 半边长
        std::vector<Vertex> verts;
        verts.reserve(24);
        for (int f = 0; f < 6; ++f) {
            const glm::vec3 center = offset + kFaceN[f] * h;  // 面心
            const glm::vec3 p[4] = {                           // 四角（从外看逆时针环）
                center + (-kFaceU[f] - kFaceV[f]) * h,   // 左下
                center + (+kFaceU[f] - kFaceV[f]) * h,   // 右下
                center + (+kFaceU[f] + kFaceV[f]) * h,   // 右上
                center + (-kFaceU[f] + kFaceV[f]) * h,   // 左上
            };
            for (int i = 0; i < 4; ++i)
                verts.push_back(Vertex{ p[i], c[f], faceUV[i] });
        }
        return verts;
    }

    // 方块索引：36 索引（每面 2 三角形，标准绕序，cache 友好）
    std::vector<uint32_t> buildCubeIndices() {
        std::vector<uint32_t> idx;
        idx.reserve(36);
        for (uint32_t f = 0; f < 6; ++f) {
            const uint32_t b = f * 4;
            idx.push_back(b + 0); idx.push_back(b + 1); idx.push_back(b + 2);
            idx.push_back(b + 0); idx.push_back(b + 2); idx.push_back(b + 3);
        }
        return idx;
    }


} // namespace engine::render::mesh
