// 装配期自检实现。来源：原 src/SelfTest.cpp，规则1 第二轮移入 core/（见 SelfTest.hpp 头部说明）。
#include "core/SelfTest.hpp"

#include "modhost/manifest/Manifest.hpp"
#include "modhost/sorter/ModSorter.hpp"
#include "render/ChunkMeshBuilder.hpp"
#include "render/VulkanMesh.hpp"
#include "render/assets/ModelDef.hpp"
#include "render/assets/TextureAtlas.hpp"
#include "save/SaveDir.hpp"
#include "save/SaveMenu.hpp"
#include "shared/chunk/ChunkData.hpp"
#include "shared/pick/PickRay.hpp"
#include "shared/pick/StructureRay.hpp"

#include <cmath>
#include <filesystem>
#include <GLFW/glfw3.h>
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
// 存档序列化：ChunkData round-trip。四种位宽/内容各存一次再读回，逐格比对。
// 这是"存档格式"的唯一自动防线 —— 格式一改这里就会炸，比手动开游戏验快得多。
bool chunkSerialize() {
    using engine::shared::BlockPos;
    using engine::shared::ChunkData;

    auto roundTrip = [](const ChunkData& src, ChunkData& dst) {
        std::vector<std::uint8_t> buf;
        src.serialize(buf, 3, -1, 7, /*genVer=*/42);
        std::uint32_t genVer = 0;
        if (!ChunkData::deserialize(buf.data(), buf.size(), 3, -1, 7, dst, &genVer)) return false;
        if (genVer != 42) return false;
        return true;
    };
    auto sameAs = [](const ChunkData& a, const ChunkData& b) {
        if (a.paletteSize() != b.paletteSize()) return false;
        if (a.chunkIndexBits() != b.chunkIndexBits()) return false;
        if (a.chunkNonAirCount() != b.chunkNonAirCount()) return false;
        for (int y = 0; y < 16; ++y)
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x)
                    if (a.paletteGet(x, y, z) != b.paletteGet(x, y, z)) return false;
        return true;
    };

    // ① 全空气（走 flags 快路径：零段）
    {
        ChunkData src, dst;
        const bool ok = roundTrip(src, dst) && sameAs(src, dst) && dst.isAllAir();
        std::cout << "  [①全空] " << (ok ? "通过" : "失败") << "\n";
        if (!ok) return false;
    }

    // ② 单材质（4 位索引）
    {
        ChunkData src;
        for (int y = 0; y < 16; ++y)
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x)
                    src.paletteSet(x, y, z, 7);
        src.markDirty(0x2u);
        ChunkData dst;
        const bool ok = roundTrip(src, dst) && sameAs(src, dst) &&
                        dst.chunkIndexBits() == 4 && dst.dirty() == 0u;
        std::cout << "  [②单材质4位] " << (ok ? "通过" : "失败") << "\n";
        if (!ok) return false;    }

    // ③ 20 种材质（索引扩到 8 位）
    {
        ChunkData src;
        for (int y = 0; y < 16; ++y)
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x)
                    src.paletteSet(x, y, z, static_cast<std::uint16_t>(1 + (x + y + z) % 20));
        ChunkData dst;
        const bool ok = roundTrip(src, dst) && sameAs(src, dst) && dst.chunkIndexBits() == 8;
        std::cout << "  [③20材质8位] " << (ok ? "通过" : "失败") << "\n";
        if (!ok) return false;
    }

    // ④ 300 种材质（索引扩到 16 位）
    {
        ChunkData src;
        for (int i = 0; i < 4096; ++i) {
            const BlockPos p = ChunkData::chunkLocalOf(static_cast<std::int32_t>(i));
            src.paletteSet(p.x, p.y, p.z, static_cast<std::uint16_t>(1 + i % 300));
        }
        ChunkData dst;
        const bool ok = roundTrip(src, dst) && sameAs(src, dst) && dst.chunkIndexBits() == 16;
        std::cout << "  [④300材质16位] " << (ok ? "通过" : "失败") << "\n";
        if (!ok) return false;
    }

    // ⑤ 损坏必须被拒（checksum / magic / 坐标不符）
    {
        ChunkData src, dst;
        src.paletteSet(1, 2, 3, 5);
        std::vector<std::uint8_t> buf;
        src.serialize(buf, 0, 0, 0, 1);
        if (buf.empty()) return false;
        // 坐标不符
        if (ChunkData::deserialize(buf.data(), buf.size(), 1, 0, 0, dst)) return false;
        // 翻转正文一个字节 → checksum 不符
        std::vector<std::uint8_t> bad = buf;
        bad[bad.size() - 1] ^= 0xFF;
        if (ChunkData::deserialize(bad.data(), bad.size(), 0, 0, 0, dst)) return false;
        // 截断
        if (ChunkData::deserialize(buf.data(), buf.size() - 1, 0, 0, 0, dst)) return false;
        // magic 被改
        std::vector<std::uint8_t> badMagic = buf;
        badMagic[0] = 0;
        if (ChunkData::deserialize(badMagic.data(), badMagic.size(), 0, 0, 0, dst)) return false;
        // 原样仍可读
        if (!ChunkData::deserialize(buf.data(), buf.size(), 0, 0, 0, dst)) return false;
    }

    std::cout << "chunk 存档：全空/4位/8位/16位 round-trip + 损坏拒绝 自检通过\n";
    return true;
}
// 存档目录（SaveDir）：写 chunk → 读回 → 比对；level.json 往返；不存在/损坏都返回 false。
// 会在工作目录下建一个临时目录，验完删掉（不污染真实存档）。
bool saveDir() {
    using engine::save::LevelInfo;
    using engine::save::SaveDir;
    using engine::shared::ChunkData;

    const std::string tmpRoot = "savetest_tmp";
    SaveDir dir(tmpRoot);
    dir.removeAll();                     // 上次残留（中断的测试）
    if (!dir.ensureDirs()) return false;
    if (!dir.exists()) return false;

    // ① 无存档 → 读必须返回 false（调用方据此走生成）
    {
        ChunkData out;
        if (dir.readChunk(0, 0, 0, out)) return false;
    }

    // ② 写 → 读 → 逐格比对（含负坐标，验证文件名编码）
    {
        ChunkData src;
        for (int y = 0; y < 16; ++y)
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x)
                    src.paletteSet(x, y, z, static_cast<std::uint16_t>(1 + (x * y + z) % 5));
        const int cx = -2, cy = 1, cz = -3;
        if (!dir.writeChunk(cx, cy, cz, src)) return false;
        if (!dir.hasChunk(cx, cy, cz)) return false;
        ChunkData dst;
        if (!dir.readChunk(cx, cy, cz, dst)) return false;
        if (dst.paletteSize() != src.paletteSize()) return false;
        if (dst.chunkNonAirCount() != src.chunkNonAirCount()) return false;
        for (int y = 0; y < 16; ++y)
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x)
                    if (dst.paletteGet(x, y, z) != src.paletteGet(x, y, z)) return false;
        // 文件名放错位置必须读不出来（坐标写在头里，双重校验）
        ChunkData other;
        if (dir.readChunk(cx + 1, cy, cz, other)) return false;
    }

    // ③ level.json 往返
    {
        LevelInfo in;
        in.formatVersion = 1;
        in.generatorVersion = 7;
        in.seed = 0x1234567890ABCDEFull;
        in.presetName = "default";
        in.worldName = "world";
        in.spawnX = 0.5; in.spawnY = 6.25; in.spawnZ = -3.75;
        in.simTime = 12.5;
        if (!dir.writeLevel(in)) return false;

        LevelInfo out;
        if (!dir.readLevel(out)) return false;
        if (out.generatorVersion != 7) return false;
        if (out.seed != in.seed) return false;
        if (out.presetName != "default" || out.worldName != "world") return false;
        if (out.spawnY != 6.25 || out.spawnZ != -3.75) return false;
        if (out.simTime != 12.5) return false;
    }

    // ④ 损坏的 chunk 文件必须读不出来（截断）
    {
        ChunkData src;
        src.paletteSet(0, 0, 0, 3);
        if (!dir.writeChunk(5, 0, 5, src)) return false;
        const std::string path = dir.chunkPath(5, 0, 5);
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) return false;
        std::filesystem::resize_file(path, size > 10 ? size - 10 : 0, ec);
        if (ec) return false;
        ChunkData out;
        if (dir.readChunk(5, 0, 5, out)) return false;
    }

    const bool cleaned = dir.removeAll();
    std::cout << "存档目录：写读 chunk/level.json + 缺失与损坏拒绝 自检"
              << ((cleaned) ? "通过" : "失败") << "\n";
    return cleaned;
}
// 存档菜单（SaveMenu）：新建/选择/删除的纯逻辑。用临时目录，不碰真实存档。
// 键盘用 GLFW 键码直接喂 onKey —— 不需要窗口，也不依赖焦点。
bool saveMenu() {
    using engine::save::SaveMenu;
    const std::string root = "savemenu_tmp";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    SaveMenu menu(root);
    // ① 空目录：无条目，ENTER 无效（提示而不是离开）
    if (!menu.empty()) return false;
    if (menu.onKey(GLFW_KEY_ENTER) != SaveMenu::Result::None) return false;

    // ② 新建两个存档（同一秒内连按 → 第二个带后缀，不能覆盖）
    const std::string a = menu.createNewSave();
    if (a.empty()) return false;
    const std::string b = menu.createNewSave();
    if (b.empty() || b == a) return false;

    // ③ 非存档目录不能被当成存档（只有 chunks/ 或 level.json 才算）
    std::filesystem::create_directories(std::filesystem::path(root) / "not_a_save", ec);
    menu.refresh();
    if (menu.entries().size() != 2) return false;

    // ④ 选择：DOWN/UP 都在范围内循环
    const std::string first = menu.selectedName();
    menu.onKey(GLFW_KEY_DOWN);
    if (menu.selectedName() == first) return false;
    menu.onKey(GLFW_KEY_UP);
    if (menu.selectedName() != first) return false;

    // ⑤ ENTER → Chosen，且名字就是选中的那个
    if (menu.onKey(GLFW_KEY_ENTER) != SaveMenu::Result::Chosen) return false;
    if (menu.selectedName() != first) return false;

    // ⑥ 删除需要两次确认：第一次只提示，第二次才真删
    const std::size_t before = menu.entries().size();
    if (menu.onKey(GLFW_KEY_X) != SaveMenu::Result::None) return false;
    if (menu.entries().size() != before) return false;      // 未删
    menu.onKey(GLFW_KEY_X);
    if (menu.entries().size() != before - 1) return false;   // 删了

    // ⑦ ESC → Quit
    if (menu.onKey(GLFW_KEY_ESCAPE) != SaveMenu::Result::Quit) return false;

    std::filesystem::remove_all(root, ec);
    std::cout << "存档菜单：新建/选择/两次确认删除/退出 自检通过\n";
    return true;
}
// 图集 UV 边界：每个格子的 UV 矩形**必须整块落在该格内**（最近邻采样也不能越界）。
// 这锁住"方块边缘冒紫色短线"那类 bug —— 根因是 UV 正好压在格边界，采样取到隔壁格
// 的 default.png（品红+黑缺失纹理标记）。用纯数学函数验证，不需要真图集文件。
bool atlasUV() {
    // 图集采用"每格四周 1px 间隔 + 边缘挤出"方案：格内容 = 16px，格距 = 18px。
    // 验证：UV 矩形换算成纹素范围后，必须**恰好等于**格内容那 16×16 像素
    // （左边从格内第一像素起、右边到格内最后一像素止）。
    // 这样边界上的采样只会落到"本格边缘的挤出复制品"，不会取到隔壁格。
    const std::uint32_t tile = 16;
    const std::uint32_t pad = 1;
    for (std::uint32_t grid = 1; grid <= 8; ++grid) {
        const std::uint32_t size = (tile + 2 * pad) * grid;
        for (std::uint32_t row = 0; row < grid; ++row) {
            for (std::uint32_t col = 0; col < grid; ++col) {
                const glm::vec4 r = engine::assets::atlasSlotUVPure(grid, size, tile, pad, col, row);
                const auto px = [size](float u) { return static_cast<std::uint32_t>(u * static_cast<float>(size) + 0.5f); };
                const std::uint32_t x0 = px(r.x), x1 = px(r.z);
                const std::uint32_t y0 = px(r.y), y1 = px(r.w);
                const std::uint32_t expectX = col * (tile + 2 * pad) + pad;
                const std::uint32_t expectY = row * (tile + 2 * pad) + pad;
                if (x0 != expectX || y0 != expectY) return false;
                if (x1 != expectX + tile || y1 != expectY + tile) return false;
            }
        }
    }
    std::cout << "图集 UV：1×1..8×8 各格 UV 恰好覆盖格内容（1px 间隔不被采样）自检通过\n";
    return true;
}

// 拾取裁决（PickResolver）：两张射线结果 → 唯一动作。
// 这是"鼠标点击到底该打方块还是打刚体"的唯一规则表，纯逻辑、可完全自动化验证。
bool pickResolve() {
    using namespace engine::shared::pick;

    auto makeBlock = [](double t) {
        PickHit h; h.kind = HitKind::Block; h.t = t;
        h.bx = 4; h.by = 5; h.bz = 6; h.px = 4; h.py = 6; h.pz = 6;
        h.blockType = 3;
        return h;
    };
    auto makeBody = [](double t) {
        PickHit h; h.kind = HitKind::Body; h.t = t;
        h.structureId = 7; h.lx = 0; h.ly = 1; h.lz = 0; h.ny = 1;
        return h;
    };
    auto none = [] { PickHit h; return h; };

    // ① 刚体更近（t=3）→ 左键拆刚体 / 右键放刚体
    if (resolve(makeBlock(5.0), makeBody(3.0), false, true, 9).kind != PickActionKind::BreakBodyBlock) return false;
    if (resolve(makeBlock(5.0), makeBody(3.0), false, false, 9).kind != PickActionKind::PlaceBodyBlock) return false;

    // ② 方块更近（t=2）→ 打方块
    if (resolve(makeBlock(2.0), makeBody(6.0), false, true, 9).kind != PickActionKind::BreakBlock) return false;
    if (resolve(makeBlock(2.0), makeBody(6.0), false, false, 9).kind != PickActionKind::PlaceBlock) return false;

    // ③ 近等距（浮点噪声量级）→ 确定地判给方块
    if (resolve(makeBlock(4.0), makeBody(4.0 - 1e-7), false, true, 9).kind != PickActionKind::BreakBlock) return false;

    // ④ 只有刚体命中 → 刚体
    if (resolve(none(), makeBody(3.0), false, true, 9).kind != PickActionKind::BreakBodyBlock) return false;

    // ⑤ 只有方块命中 → 方块
    if (resolve(makeBlock(2.0), none(), false, true, 9).kind != PickActionKind::BreakBlock) return false;

    // ⑥ 都没命中 → 无动作
    if (resolve(none(), none(), false, true, 9).kind != PickActionKind::None) return false;

    // ⑦ 物理探测超时 → 即使刚体命中也要走方块（点击不能因对端慢而丢失）
    if (resolve(makeBlock(5.0), makeBody(1.0), true, true, 9).kind != PickActionKind::BreakBlock) return false;

    // ⑧ 右键放置要把 placeType 带进动作
    {
        const PickAction a = resolve(makeBlock(2.0), none(), false, false, 42);
        if (a.kind != PickActionKind::PlaceBlock || a.block.blockType != 42) return false;
    }

    std::cout << "拾取裁决：刚体/方块/近等距/超时/双空 自检通过\n";
    return true;
}
// 结构局部射线：把射线变到刚体局部空间后，用"单位立方体 DDA"求交。
// 这是业务侧同步裁决的核心几何（不再跨线程问物理），必须能脱机验证。
bool structureRay() {
    using namespace engine::shared::pick;

    // 3×3×3 实心块，局部坐标 -1..1（与演示刚体同构）
    auto solid = [](int x, int y, int z) {
        return x >= -1 && x <= 1 && y >= -1 && y <= 1 && z >= -1 && z <= 1;
    };

    // ① 沿 -z 打：从 z=+5 出发，应先命中 z=+1 的面，法线 +z
    {
        const LocalHit h = raycastLocalVoxel(0.0, 0.0, 5.0, 0.0, 0.0, -1.0, 8.0, solid);
        if (!h.hit || h.x != 0 || h.y != 0 || h.z != 1 || h.nz != 1) return false;
        if (std::fabs(h.t - 3.0) > 1e-9) return false;   // 格 z=1 占 [1,2]，从 z=5 入射距离 = 3
    }
    // ② 沿 -x 打：命中 x=+1，法线 +x
    {
        const LocalHit h = raycastLocalVoxel(5.0, 0.0, 0.0, -1.0, 0.0, 0.0, 8.0, solid);
        if (!h.hit || h.x != 1 || h.nx != 1) return false;
    }
    // ③ 沿 +y 从下方打：命中 y=-1，法线 -y
    {
        const LocalHit h = raycastLocalVoxel(0.0, -5.0, 0.0, 0.0, 1.0, 0.0, 8.0, solid);
        if (!h.hit || h.y != -1 || h.ny != -1) return false;
    }
    // ④ 打偏（差 3 格）：不该命中
    {
        const LocalHit h = raycastLocalVoxel(3.0, 3.0, 5.0, 0.0, 0.0, -1.0, 8.0, solid);
        if (h.hit) return false;
    }
    // ⑤ 距离不够（隔着 10 格）：不该命中
    {
        const LocalHit h = raycastLocalVoxel(0.0, 0.0, 12.0, 0.0, 0.0, -1.0, 8.0, solid);
        if (h.hit) return false;
    }
    // ⑥ 起点在实体内：t=0 命中起点格
    {
        const LocalHit h = raycastLocalVoxel(0.5, 0.5, 0.5, 0.0, 0.0, -1.0, 8.0, solid);
        if (!h.hit || h.t != 0.0 || h.x != 0) return false;
    }
    // ⑦ 斜向打：必须命中且 t 在合理范围（0 < t < 8）
    {
        const LocalHit h = raycastLocalVoxel(3.0, 3.0, 3.0, -0.5773503, -0.5773503, -0.5773503,
                                             8.0, solid);
        if (!h.hit || h.t <= 0.0 || h.t > 8.0) return false;
    }

    std::cout << "结构射线：轴向/法线/打偏/距离/起点在体内/斜向 自检通过\n";
    return true;
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
