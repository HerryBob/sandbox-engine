#include "VulkanMesh.hpp"
#include <bit>
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

    std::vector<Vertex> buildCubeVertices(const glm::vec3& offset, std::uint8_t faceMask,
                                          const glm::vec4& atlasUV) {
        // 面 UV 由图集矩形展开（Vulkan v=0 在图像顶行 → 几何上方用 v0、下方用 v1，方向正确）：
        // 左下 {u0,v1} → 右下 {u1,v1} → 右上 {u1,v0} → 左上 {u0,v0}
        const glm::vec2 faceUV[4] = {
            {atlasUV.x, atlasUV.w},
            {atlasUV.z, atlasUV.w},
            {atlasUV.z, atlasUV.y},
            {atlasUV.x, atlasUV.y}
        };
        // 颜色责任已移交纹理（片元着色器仅采样纹理，不读顶点色）。
        // 顶点色保留 {1,1,1} 白色作为占位调制因子，未来光照/调色可复用。
        const glm::vec3 color{1.0f, 1.0f, 1.0f};
        const float h = 0.5f; // 半边长
        std::vector<Vertex> verts;
        verts.reserve(static_cast<size_t>(std::popcount(faceMask))*4);
        for (int f = 0; f < 6; ++f) {
            if (!(faceMask & (1u << f))) continue;

                const glm::vec3 center = offset + kFaceN[f] * h;  // 面心
                const glm::vec3 p[4] = {                           // 四角（从外看逆时针环）
                    center + (-kFaceU[f] - kFaceV[f]) * h,   // 左下
                    center + (+kFaceU[f] - kFaceV[f]) * h,   // 右下
                    center + (+kFaceU[f] + kFaceV[f]) * h,   // 右上
                    center + (-kFaceU[f] + kFaceV[f]) * h,   // 左上
                };
                for (int i = 0; i < 4; ++i)
                    verts.push_back(Vertex{ p[i], color, faceUV[i] });
            }
        return verts;
    }
} // namespace engine::render::mesh
