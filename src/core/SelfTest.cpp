// 装配期自检实现。来源：原 src/SelfTest.cpp，规则1 第二轮移入 core/（见 SelfTest.hpp 头部说明）。
#include "core/SelfTest.hpp"

#include "modhost/manifest/Manifest.hpp"
#include "modhost/sorter/ModSorter.hpp"
#include "render/ChunkMeshBuilder.hpp"
#include "render/VulkanMesh.hpp"
#include "render/assets/ModelDef.hpp"
#include "render/assets/TextureAtlas.hpp"
#include "shared/chunk/ChunkData.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace engine::selftest {

// ChunkData 自检：坐标编解码互逆 + 调色板 4→8→16 两档扩宽后逐格内容不变、计数正确。
bool palette() {
    // ⓪ chunkIndexOf / chunkLocalOf 严格互逆（16³ 全部组合，任一处改动未同步即失败）
    for (int z = 0; z < 16; ++z)
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x) {
                const auto i = shared::ChunkData::chunkIndexOf(x, y, z);
                const auto p = shared::ChunkData::chunkLocalOf(i);
                if (p.x != x || p.y != y || p.z != z) return false;
            }

    // ① 新 chunk 全空
    shared::ChunkData blank;
    if (!blank.isAllAir() || blank.chunkNonAirCount() != 0) return false;
    if (blank.chunkIndexBits() != 4 || blank.paletteSize() != 1) return false;

    // ② 4→8 扩宽（第 17 个条目触发）
    shared::ChunkData a;
    for (int i = 0; i < 20; ++i)
        a.paletteSet(i % 16, (i / 16) % 16, 0, static_cast<shared::PaletteEntry>(2000 + i));
    if (a.chunkIndexBits() != 8 || a.paletteSize() != 21) return false;
    for (int i = 0; i < 20; ++i)
        if (a.paletteGet(i % 16, (i / 16) % 16, 0) != static_cast<shared::PaletteEntry>(2000 + i))
            return false;
    if (a.chunkNonAirCount() != 20) return false;

    // ③ 8→16 扩宽（第 257 个条目触发），写满 300 项后再逐格核对
    shared::ChunkData b;
    for (int i = 0; i < 300; ++i)
        b.paletteSet(i % 16, (i / 16) % 16, i / 256, static_cast<shared::PaletteEntry>(3000 + i));
    if (b.chunkIndexBits() != 16 || b.paletteSize() != 301) return false;
    for (int i = 0; i < 300; ++i)
        if (b.paletteGet(i % 16, (i / 16) % 16, i / 256) !=
            static_cast<shared::PaletteEntry>(3000 + i))
            return false;
    for (std::int32_t i = 300; i < shared::CHUNK_VOLUME; ++i)
        if (b.paletteGetByIndex(i) != shared::kAirEntry) return false;
    if (b.chunkNonAirCount() != 300) return false;

    // ④ 写回 Air：字典里仍留着非空气条目，判空必须逐格扫完才返回 true
    shared::ChunkData c;
    const shared::PaletteEntry five = static_cast<shared::PaletteEntry>(5);
    c.paletteSet(1, 1, 1, five);
    if (c.paletteGet(1, 1, 1) != five) return false;
    c.paletteSet(1, 1, 1, shared::kAirEntry);
    if (!c.isAllAir() || c.chunkNonAirCount() != 0) return false;

    // ⑤ 压缩：16 位档区块删掉大半方块后，字典与位宽一起降回 4 位，且逐格数据不变
    shared::ChunkData d;
    for (int i = 0; i < 300; ++i)
        d.paletteSet(i % 16, (i / 16) % 16, i / 256, static_cast<shared::PaletteEntry>(4000 + i));
    if (d.chunkIndexBits() != 16 || d.paletteSize() != 301) return false;
    for (int i = 2; i < 300; ++i)
        d.paletteSet(i % 16, (i / 16) % 16, i / 256, shared::kAirEntry);
    if (d.chunkNonAirCount() != 2) return false;
    if (d.paletteSize() != 301) return false;   // 压缩前字典仍是 301 项（Air 以内均为闲置）
    d.chunkCompact();
    if (d.chunkIndexBits() != 4 || d.paletteSize() != 3) return false;   // Air + 两项在用
    if (d.chunkNonAirCount() != 2) return false;
    if (d.paletteGet(0, 0, 0) != static_cast<shared::PaletteEntry>(4000)) return false;
    if (d.paletteGet(1, 0, 0) != static_cast<shared::PaletteEntry>(4001)) return false;
    for (int i = 2; i < 300; ++i)
        if (d.paletteGet(i % 16, (i / 16) % 16, i / 256) != shared::kAirEntry) return false;
    return true;
}

// 元素级旋转：绕轴旋转公式 + 原点不动，纯数学自检（不依赖图集与窗口）
bool modelRotation() {
    using engine::assets::applyModelRotation;
    using engine::assets::ModelRotation;

    // ① 绕 Z 轴 45°：点 (16,8,8) 绕原点 (8,8,8) → (8+8cos45, 8+8sin45, 8)
    ModelRotation rz;
    rz.origin = glm::vec3{8.0f, 8.0f, 8.0f};
    rz.axis = 'z';
    rz.angle = 45.0f;
    const glm::vec3 a = applyModelRotation(glm::vec3{16.0f, 8.0f, 8.0f}, rz);
    const float e = 8.0f + 8.0f * 0.70710678f;
    if (std::fabs(a.x - e) > 0.01f || std::fabs(a.y - e) > 0.01f || std::fabs(a.z - 8.0f) > 0.01f)
        return false;

    // ② 旋转原点自身不动
    const glm::vec3 o = applyModelRotation(rz.origin, rz);
    if (std::fabs(o.x - 8.0f) > 0.01f || std::fabs(o.y - 8.0f) > 0.01f || std::fabs(o.z - 8.0f) > 0.01f)
        return false;

    // ③ 绕 Y 轴 90°：+X 转到 -Z（右手系）
    ModelRotation ry;
    ry.origin = glm::vec3{8.0f, 8.0f, 8.0f};
    ry.axis = 'y';
    ry.angle = 90.0f;
    const glm::vec3 b = applyModelRotation(glm::vec3{16.0f, 8.0f, 8.0f}, ry);
    if (std::fabs(b.x - 8.0f) > 0.01f || std::fabs(b.y - 8.0f) > 0.01f || std::fabs(b.z - 0.0f) > 0.01f)
        return false;

    // ④ 绕 X 轴 90°：+Y 转到 +Z
    ModelRotation rx;
    rx.origin = glm::vec3{8.0f, 8.0f, 8.0f};
    rx.axis = 'x';
    rx.angle = 90.0f;
    const glm::vec3 d = applyModelRotation(glm::vec3{8.0f, 16.0f, 8.0f}, rx);
    if (std::fabs(d.x - 8.0f) > 0.01f || std::fabs(d.y - 8.0f) > 0.01f || std::fabs(d.z - 16.0f) > 0.01f)
        return false;

    return true;
}

// 元素级旋转的几何落点：直接跑 buildModelVertices，验旋转后顶点的包围盒尺寸。
// 公式自检只证明数学，这里证明"模型里的 rotation 字段确实进到了顶点"。
bool modelGeometry() {
    using engine::render::Vertex;

    engine::assets::ModelDef def;
    engine::assets::ModelElement el;
    el.from = glm::vec3{0.0f, 0.0f, 0.0f};
    el.to   = glm::vec3{16.0f, 16.0f, 16.0f};
    for (int f = 0; f < 6; ++f) {
        el.faces[f].texture = "default";
        el.faces[f].cullFace = 0;          // 永不剔 → 6 面全生成
    }
    el.rotation.origin = glm::vec3{8.0f, 8.0f, 8.0f};
    el.rotation.axis = 'z';
    el.rotation.angle = 45.0f;
    def.elements.push_back(el);

    engine::assets::TextureAtlas emptyAtlas;   // 空图集：纹理走兜底格，不影响位置
    std::vector<Vertex> verts;
    engine::render::mesh::buildModelVertices(verts, glm::vec3{0.0f, 0.0f, 0.0f}, def, 0u, emptyAtlas);
    if (verts.size() != 24) return false;      // 6 面 × 4 顶点

    // 绕 Z 转 45°：X/Y 跨度 16√2 ≈ 22.627，Z 跨度仍 16；÷16 后 → 1.414 / 1.0
    glm::vec3 lo{1e30f, 1e30f, 1e30f};
    glm::vec3 hi{-1e30f, -1e30f, -1e30f};
    for (const Vertex& v : verts) {
        lo = glm::min(lo, v.pos);
        hi = glm::max(hi, v.pos);
    }
    const glm::vec3 span = hi - lo;
    const float diag = 22.6274f * 0.0625f;
    if (std::fabs(span.x - diag) > 0.05f) return false;
    if (std::fabs(span.y - diag) > 0.05f) return false;
    if (std::fabs(span.z - 1.0f) > 0.05f) return false;

    // 同一元素不旋转：三轴跨度都应是 1.0（确认走的是同一条路径的恒等分支）
    def.elements.front().rotation.angle = 0.0f;
    std::vector<Vertex> plain;
    engine::render::mesh::buildModelVertices(plain, glm::vec3{0.0f, 0.0f, 0.0f}, def, 0u, emptyAtlas);
    if (plain.size() != 24) return false;
    glm::vec3 lo2{1e30f, 1e30f, 1e30f};
    glm::vec3 hi2{-1e30f, -1e30f, -1e30f};
    for (const Vertex& v : plain) {
        lo2 = glm::min(lo2, v.pos);
        hi2 = glm::max(hi2, v.pos);
    }
    const glm::vec3 span2 = hi2 - lo2;
    if (std::fabs(span2.x - 1.0f) > 0.01f) return false;
    if (std::fabs(span2.y - 1.0f) > 0.01f) return false;
    if (std::fabs(span2.z - 1.0f) > 0.01f) return false;

    return true;
}

// 刚体网格：3×3×3 = 27 方块，内部接触面全剔除 → 外层 6×9 = 54 面 = 216 顶点。
// 空图集走兜底 UV、空注册表走立方体兜底——验的是"只按刚体内部剔除"这条逻辑。
bool bodyMesh() {
    using engine::shared::Block;
    using engine::shared::BlockPos;
    using engine::shared::BlockType;

    std::vector<Block> blocks;
    blocks.reserve(27);
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            for (int z = -1; z <= 1; ++z)
                blocks.push_back(Block{static_cast<BlockType>(1), BlockPos{x, y, z}, 0});

    engine::assets::TextureAtlas emptyAtlas;   // 空图集：纹理走兜底格，不影响几何
    const engine::render::mesh::BodyMeshData body =
        engine::render::mesh::buildBodyVertices(blocks, emptyAtlas);

    const std::size_t faces = body.indices.size() / 6;
    const std::size_t verts = body.vertices.size();
    const bool ok = (faces == 54 && verts == 216);

    std::cout << "刚体网格：3×3×3 面=" << faces << " 顶点=" << verts
              << (ok ? " 自检通过" : " 自检失败") << "\n";
    return ok;
}

// 世界 chunk 网格（ChunkMeshBuilder）：面剔除与顶点生成是纯 CPU 的，可在这里单测。
// 三个用例：① 空输入 ② 单方块 6 面 ③ 3×3×3 实心 54 面（与刚体网格同一判据）。
bool chunkMesh() {
    using engine::shared::Block;
    using engine::shared::BlockPos;
    using engine::shared::BlockType;
    using engine::render::ChunkMeshBuilder;
    using engine::render::ChunkMeshData;

    engine::assets::TextureAtlas emptyAtlas;   // 空图集：纹理走兜底格，不影响几何

    // ① 空输入 → 空网格
    {
        std::vector<Block> none;
        std::unordered_set<BlockPos> solid;
        const ChunkMeshData d = ChunkMeshBuilder::buildChunk(none, solid, emptyAtlas);
        if (!d.empty()) return false;
    }

    // ② 单个孤立方块 → 6 面 / 24 顶点（六面全可见，无邻居剔除）
    {
        std::vector<Block> one{Block{static_cast<BlockType>(1), BlockPos{0, 0, 0}, 0}};
        std::unordered_set<BlockPos> solid{BlockPos{0, 0, 0}};   // 手工集合，不依赖注册表
        const ChunkMeshData d = ChunkMeshBuilder::buildChunk(one, solid, emptyAtlas);
        const std::size_t faces = d.indices.size() / 6;
        if (faces != 6 || d.verts.size() != 24) return false;
    }

    // ③ 3×3×3 实心 → 内部接触面全剔除 → 54 面 / 216 顶点
    {
        std::vector<Block> blocks;
        blocks.reserve(27);
        std::unordered_set<BlockPos> solid;   // 手工建集合：自检不依赖方块注册表是否已加载
        solid.reserve(27);
        for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
                for (int z = -1; z <= 1; ++z) {
                    blocks.push_back(Block{static_cast<BlockType>(1), BlockPos{x, y, z}, 0});
                    solid.insert(BlockPos{x, y, z});
                }
        const ChunkMeshData d = ChunkMeshBuilder::buildChunk(blocks, solid, emptyAtlas);
        const std::size_t faces = d.indices.size() / 6;
        const std::size_t verts = d.verts.size();
        const bool ok = (faces == 54 && verts == 216);
        std::cout << "chunk 网格：3×3×3 面=" << faces << " 顶点=" << verts
                  << (ok ? " 自检通过" : " 自检失败") << "\n";
        return ok;
    }
}
// 模组排序：跨批次依赖方向 + 缺失依赖 + 同批环，纯内存自检（不碰磁盘与 DLL）。
bool modSorter() {
    using engine::modhost::ModManifest;
    using engine::modhost::ModPhase;
    using engine::modhost::ModSorter;
    using engine::modhost::SortResult;

    // ① 批次倒序：builtin 依赖 normal → 必须拒绝（normal 在它之后才加载）
    {
        std::vector<ModManifest> items(2);
        items[0].id = "a_early";
        items[0].phase = ModPhase::Builtin;
        items[0].dependencies.push_back("b_late");
        items[1].id = "b_late";
        items[1].phase = ModPhase::Normal;

        const SortResult r = ModSorter::sort(items);
        if (r.rejected.size() != 1 || r.rejected[0].index != 0) return false;
        if (r.order.size() != 1 || r.order[0] != 1) return false;
    }

    // ② 正常跨批：normal 依赖 builtin → 放行，且 builtin 先出队
    {
        std::vector<ModManifest> items(2);
        items[0].id = "a_normal";
        items[0].phase = ModPhase::Normal;
        items[0].dependencies.push_back("b_builtin");
        items[1].id = "b_builtin";
        items[1].phase = ModPhase::Builtin;

        const SortResult r = ModSorter::sort(items);
        if (!r.rejected.empty()) return false;
        if (r.order.size() != 2) return false;
        if (r.order[0] != 1 || r.order[1] != 0) return false;
    }

    // ③ 依赖的 id 不存在 → 拒绝
    {
        std::vector<ModManifest> items(1);
        items[0].id = "a_ghost";
        items[0].dependencies.push_back("nobody");

        const SortResult r = ModSorter::sort(items);
        if (r.rejected.size() != 1 || r.rejected[0].index != 0) return false;
        if (!r.order.empty()) return false;
    }

    // ④ 同批次环：a ↔ b → 双方都拒绝
    {
        std::vector<ModManifest> items(2);
        items[0].id = "a_ring";
        items[0].dependencies.push_back("b_ring");
        items[1].id = "b_ring";
        items[1].dependencies.push_back("a_ring");

        const SortResult r = ModSorter::sort(items);
        if (r.rejected.size() != 2) return false;
        if (!r.order.empty()) return false;
    }

    return true;
}

} // namespace engine::selftest
