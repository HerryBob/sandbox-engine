#include "worldgen/generator/WorldGenerator.hpp"

#include "business/World.hpp"
#include "registry/BlockRegistry.hpp"
#include "shared/block/Block.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace engine::worldgen {

namespace {

// 1a 生成区域：x,z ∈ [-kGenRadius, kGenRadius-1]（48×48 列），y ∈ [0,63]（4 个 chunk 竖排）。
// 1b 由加载窗口接管（届时随窗口移动，本常量随之下线）。
constexpr int kGenRadius = 24;
constexpr int kGenMinY = 0;
constexpr int kGenMaxY = 63;

// 生成失败统一出口
bool fail(const char* msg) {
    std::fprintf(stderr, "[WorldGen] %s\n", msg);
    return false;
}

} // namespace

WorldGenerator::WorldGenerator(engine::business::World& world, const WorldPreset& preset)
    : world_(world), preset_(preset) {}

bool WorldGenerator::generate(Stats* stats) {
    // ---- 1. 预设前置校验 ----
    if (!preset_.heightmap) return fail("高度图噪声缺失（预设未经 PresetLoader 实例化）");
    if (preset_.strata.empty()) return fail("地层表为空");

    // ---- 2. 地层表解析：名字 → BlockType（走方块注册表名字查询，数据驱动）----
    struct Layer {
        std::uint8_t type = 0;   // BlockType 值
        int depthMin = 0;
        int depthMax = 0;
    };
    std::vector<Layer> layers;
    layers.reserve(preset_.strata.size());
    for (const Stratum& s : preset_.strata) {
        const std::uint8_t id = engine::registry::findBlockIdByName(s.block);
        if (id == 0xFF) {
            std::fprintf(stderr, "[WorldGen] 地层引用未注册方块：%s\n", s.block.c_str());
            return false;
        }
        if (s.depthMin < 0 || s.depthMin > s.depthMax) {
            std::fprintf(stderr, "[WorldGen] 地层深度区间非法：%s [%d,%d]\n",
                         s.block.c_str(), s.depthMin, s.depthMax);
            return false;
        }
        layers.push_back(Layer{id, s.depthMin, s.depthMax});
    }

    // ---- 3. 覆盖连续性校验：从深度 0 起逐层衔接、无重叠/断档（否则会生成空气夹层）----
    std::sort(layers.begin(), layers.end(),
              [](const Layer& a, const Layer& b) { return a.depthMin < b.depthMin; });
    if (layers.front().depthMin != 0) return fail("地层表未覆盖地表深度 0");
    for (std::size_t i = 1; i < layers.size(); ++i) {
        if (layers[i].depthMin != layers[i - 1].depthMax + 1) {
            std::fprintf(stderr, "[WorldGen] 地层深度断档/重叠：第 %zu 层与上一层不衔接\n", i);
            return false;
        }
    }
    const int maxDepth = layers.back().depthMax;

    // ---- 4. 逐列采样高度并按"相对地表深度"填层 ----
    std::vector<engine::shared::Block> blocks;
    blocks.reserve(static_cast<std::size_t>(kGenRadius * 2) * (kGenRadius * 2) * 48);

    const int surfaceBase = static_cast<int>(std::lround(preset_.baseHeight));
    const int amplitude   = static_cast<int>(std::lround(preset_.amplitude));
    // 地表可用上限：y ≤ 63 且 depth = surfaceY - y 必须落进地层表（≤ maxDepth）
    const int surfaceMax = std::min(kGenMaxY, maxDepth);
    if (surfaceMax < kGenMinY) return fail("地层表深度范围无法容纳任何地表高度");

    int observedMin = surfaceMax;
    int observedMax = kGenMinY;
    std::size_t columns = 0;

    for (int x = -kGenRadius; x < kGenRadius; ++x) {
        for (int z = -kGenRadius; z < kGenRadius; ++z) {
            // 噪声纯函数：x/z 未缩放世界坐标，seed 显式传入（流式重建命脉）
            const double n = preset_.heightmap->sample(
                static_cast<double>(x), static_cast<double>(z), preset_.seed);
            int surfaceY = surfaceBase +
                static_cast<int>(std::lround(n * static_cast<double>(amplitude)));
            surfaceY = std::clamp(surfaceY, kGenMinY, surfaceMax);
            observedMin = std::min(observedMin, surfaceY);
            observedMax = std::max(observedMax, surfaceY);

            for (int y = kGenMinY; y <= surfaceY; ++y) {
                const int depth = surfaceY - y;   // 相对地表深度：0 = 地表
                std::uint8_t type = 0;
                bool found = false;
                for (const Layer& l : layers) {
                    if (depth >= l.depthMin && depth <= l.depthMax) {
                        type = l.type;
                        found = true;
                        break;
                    }
                }
                if (!found) continue;   // 连续性已校验，理论上不可达；兜底跳过
                blocks.push_back(engine::shared::Block{
                    static_cast<engine::shared::BlockType>(type),
                    engine::shared::BlockPos{x, y, z},
                    -1});
            }
            ++columns;
        }
    }

    // ---- 5. 批量写入：一次锁、直写 chunk、逐 chunk 标脏 ----
    world_.generateBlocks(blocks);

    if (stats) {
        stats->columns = columns;
        stats->blocks = blocks.size();
        stats->minSurfaceY = observedMin;
        stats->maxSurfaceY = observedMax;
    }
    return true;
}

} // namespace engine::worldgen
