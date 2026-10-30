#include "VulkanMesh.hpp"
#include <bit>
namespace engine::render::mesh {
    using engine::assets::TextureAtlas;
    using engine::assets::AtlasSlot;
    using engine::assets::ModelFace;

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

    // ---- 数据驱动模型：按 ModelDef 生成顶点（多元素 + 逐面纹理/剔除）----
    std::vector<Vertex> buildModelVertices(const glm::vec3& offset,
                                           const engine::assets::ModelDef& model,
                                           std::uint8_t faceMask, const TextureAtlas& atlas) {
        const glm::vec3 color{1.0f, 1.0f, 1.0f};
        std::vector<Vertex> verts;
        for (const auto& el : model.elements) {
            // 16 单位 → 世界：from/to ÷16 再平移半格（元素左下角对齐格角）
            const glm::vec3 lo = offset + (el.from * 0.0625f) - 0.5f;
            const glm::vec3 hi = offset + (el.to   * 0.0625f) - 0.5f;
            const glm::vec3 c  = (lo + hi) * 0.5f;   // 元素中心
            const glm::vec3 he = (hi - lo) * 0.5f;   // 每轴半长
            // 沿指定轴缩放半长（kFace 表每轴只有一个非零分量 → 逐分量乘即取对应半长）
            const auto scaled = [&](const glm::vec3& axis) {
                return glm::vec3(axis.x * he.x, axis.y * he.y, axis.z * he.z);
            };
            for (int f = 0; f < 6; ++f) {
                const ModelFace& mf = el.faces[f];
                // 剔除：cullFace=d → 该方向邻块挡住（faceMask 位 d-1 == 0）则剔；
                // cullFace=0（永不剔，如玻璃内部面）→ 恒画。
                if (mf.cullFace != 0 && (faceMask & (1u << (mf.cullFace - 1))) == 0)
                    continue;
                // 纹理 → UV：面纹理名 → 图集格 → UV 矩形（未注册/空 → fallback 兜底格）
                glm::vec4 uvRect = TextureAtlas::fallbackUV();
                if (!mf.texture.empty()) {
                    AtlasSlot slot;
                    if (atlas.slotOf(mf.texture, slot))
                        uvRect = TextureAtlas::slotUV(slot);
                }
                const glm::vec2 faceUV[4] = {
                    {uvRect.x, uvRect.w}, {uvRect.z, uvRect.w},
                    {uvRect.z, uvRect.y}, {uvRect.x, uvRect.y}};
                // 面心 + 四角（主轴/副轴按各自半长缩放，u×v=n 绕序保持朝外）
                const glm::vec3 center = c + scaled(kFaceN[f]);
                const glm::vec3 p[4] = {
                    center + (-scaled(kFaceU[f]) - scaled(kFaceV[f])),   // 左下
                    center + (+scaled(kFaceU[f]) - scaled(kFaceV[f])),   // 右下
                    center + (+scaled(kFaceU[f]) + scaled(kFaceV[f])),   // 右上
                    center + (-scaled(kFaceU[f]) + scaled(kFaceV[f])),   // 左上
                };
                for (int i = 0; i < 4; ++i)
                    verts.push_back(Vertex{ p[i], color, faceUV[i] });
            }
        }
        return verts;
    }
} // namespace engine::render::mesh
