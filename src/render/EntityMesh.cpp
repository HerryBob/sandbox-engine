#include "EntityMesh.hpp"

namespace engine::render::mesh {

namespace {

// 面定义表：法线 n + 屏幕右 u + 屏幕上 v（u×v = n，与 VulkanMesh.cpp 同源）
// 索引 0..5 = +X -X +Y -Y +Z -Z
const glm::vec3 kFaceN[6] = {
    { 1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0,-1, 0}, {0, 0, 1}, {0, 0,-1}
};
const glm::vec3 kFaceU[6] = {
    {0,0,-1}, {0,0,1}, {1,0,0}, {1,0,0}, {1,0,0}, {-1,0,0}
};
const glm::vec3 kFaceV[6] = {
    {0,1,0}, {0,1,0}, {0,0,-1}, {0,0,1}, {0,1,0}, {0,1,0}
};

} // namespace

EntityMeshData buildEntityVertices(const engine::assets::EntityModel& model) {
    EntityMeshData out;
    out.bones.resize(model.bones.size());

    // UV 像素 → 0..1（纹理 v=0 在图像顶行，与 bbmodel 导出的 uv 同向）
    const float invW = (model.texWidth  > 0) ? 1.0f / static_cast<float>(model.texWidth)  : 0.0f;
    const float invH = (model.texHeight > 0) ? 1.0f / static_cast<float>(model.texHeight) : 0.0f;

    for (std::size_t bi = 0; bi < model.bones.size(); ++bi) {
        const auto& bone = model.bones[bi];
        auto& bm = out.bones[bi];
        bm.vertices.reserve(bone.cubes.size() * 24);
        bm.indices.reserve(bone.cubes.size() * 36);

        for (const auto& cube : bone.cubes) {
            const glm::vec3 c16  = (cube.from + cube.to) * 0.5f;
            const glm::vec3 he16 = (cube.to - cube.from) * 0.5f;

            for (int f = 0; f < 6; ++f) {
                const auto& uv = cube.faces[f];
                // 四角 UV：左下(u0,v1) 右下(u1,v1) 右上(u1,v0) 左上(u0,v0)
                // bbmodel 存的是 [u0,v0,u1,v1]，v 向下，所以 v1 是「下」边
                const glm::vec2 faceUV[4] = {
                    {uv.u0 * invW, uv.v1 * invH},
                    {uv.u1 * invW, uv.v1 * invH},
                    {uv.u1 * invW, uv.v0 * invH},
                    {uv.u0 * invW, uv.v0 * invH},
                };

                // 面心 = 立方体中心 + 法线 × 该轴半长
                const glm::vec3 c16f{
                    c16.x + kFaceN[f].x * he16.x,
                    c16.y + kFaceN[f].y * he16.y,
                    c16.z + kFaceN[f].z * he16.z};
                const glm::vec3 su{kFaceU[f].x * he16.x, kFaceU[f].y * he16.y, kFaceU[f].z * he16.z};
                const glm::vec3 sv{kFaceV[f].x * he16.x, kFaceV[f].y * he16.y, kFaceV[f].z * he16.z};

                // 从外面看逆时针：左下 → 右下 → 右上 → 左上
                const glm::vec3 q16[4] = {
                    c16f - su - sv,
                    c16f + su - sv,
                    c16f + su + sv,
                    c16f - su + sv,
                };

                const std::uint32_t base = static_cast<std::uint32_t>(bm.vertices.size());
                for (int i = 0; i < 4; ++i)
                    bm.vertices.push_back(Vertex{q16[i] * 0.0625f, glm::vec3(1.0f), faceUV[i]});

                bm.indices.push_back(base + 0);
                bm.indices.push_back(base + 1);
                bm.indices.push_back(base + 2);
                bm.indices.push_back(base + 0);
                bm.indices.push_back(base + 2);
                bm.indices.push_back(base + 3);
            }
        }
    }
    return out;
}

} // namespace engine::render::mesh
