#include "worldgen/generator/ColumnGenerator.hpp"

#include "core/Log.hpp"
#include "registry/BlockRegistry.hpp"
#include "shared/block/Block.hpp"
#include "worldgen/noise/NoiseRegistry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace engine::worldgen {

namespace {

// 生成失败统一出口（与 1a 同格式，便于对比日志）
bool fail(const char* msg) {
    LOG_ERROR("ColumnGen", msg);
    return false;
}

} // namespace

ColumnGenerator::ColumnGenerator(WorldPreset preset)
    : preset_(std::move(preset)) {}

bool ColumnGenerator::prepare() {
    // ---- 1. 预设前置校验 ----
    if (preset_.heightType.empty())
        return fail("高度场 provider 未指定（heightmap.noise.type）");
    if (preset_.strata.empty()) return fail("地层表为空");

    // ---- 2. 地层表解析：注册名 → BlockType（数据驱动，走方块注册表名字查询）----
    layers_.clear();
    layers_.reserve(preset_.strata.size());
    for (const Stratum& s : preset_.strata) {
        const std::uint16_t id = engine::registry::findBlockIdByName(s.block);
        if (id == 0xFFFF) {
            LOG_ERROR("ColumnGen", "地层引用未注册方块：" + s.block);
            return false;
        }
        if (s.depthMin < 0 || s.depthMin > s.depthMax) {
            LOG_ERROR("ColumnGen", "地层深度区间非法：" + s.block + " [" +
                                   std::to_string(s.depthMin) + "," + std::to_string(s.depthMax) + "]");
            return false;
        }
        layers_.push_back(Layer{id, s.depthMin, s.depthMax});
    }

    // ---- 3. 覆盖连续性校验：从深度 0 起逐层衔接、无重叠/断档 ----
    std::sort(layers_.begin(), layers_.end(),
              [](const Layer& a, const Layer& b) { return a.depthMin < b.depthMin; });
    if (layers_.front().depthMin != 0) return fail("地层表未覆盖地表深度 0");
    for (std::size_t i = 1; i < layers_.size(); ++i) {
        if (layers_[i].depthMin != layers_[i - 1].depthMax + 1) {
            LOG_ERROR("ColumnGen", "地层深度断档/重叠：第 " + std::to_string(i) + " 层与上一层不衔接");
            return false;
        }
    }
    const int maxDepth = layers_.back().depthMax;

    // ---- 4. 高度映射参数预计算 ----
    surfaceBase_ = static_cast<int>(std::lround(preset_.baseHeight));
    amplitude_   = static_cast<int>(std::lround(preset_.amplitude));
    // 地表可用上限：y ≤ 63 且 depth = surfaceY - y 必须落进地层表（≤ maxDepth）
    surfaceMax_ = std::min(kGenMaxY, maxDepth);
    if (surfaceMax_ < kGenMinY) return fail("地层表深度范围无法容纳任何地表高度");

    // ---- 5. 创建高度场句柄（所有校验通过后最后做，失败不留半成品）----
    // 参数数组只在 make 调用期间有效：key / str 都指向 preset_ 内的 string，
    // provider 必须自己拷贝需要长期保留的值（契约见 modapi/noise_api.h）。
    std::vector<modapi::NoiseParamC> params;
    params.reserve(preset_.heightParams.size() + 1U);
    if (!preset_.heightBase.empty()) {
        params.push_back(modapi::NoiseParamC{"base", preset_.heightBase.c_str(), 0.0});
    }
    for (const HeightParam& p : preset_.heightParams) {
        params.push_back(modapi::NoiseParamC{
            p.key.c_str(), p.str.empty() ? nullptr : p.str.c_str(), p.num});
    }

    NoiseHandle created;
    if (!NoiseRegistry::make(preset_.heightType.c_str(),
                             params.empty() ? nullptr : params.data(),
                             static_cast<std::int32_t>(params.size()),
                             created)) {
        LOG_ERROR("ColumnGen", "高度场 provider 创建失败：" + preset_.heightType);
        return false;
    }
    height_ = std::move(created);

    prepared_ = true;
    return true;
}

void ColumnGenerator::generateColumn(const ColPos& col, ColumnBlocks& out) const {
    // 整体重置：复用同一 ColumnBlocks 时不能残留上一列的方块（16KB memset，可忽略）
    out = ColumnBlocks{};
    out.pos = col;
    if (!prepared_) return;   // 未 prepare：返回空列（调用方应检查 prepared()）

    int observedMin = kGenMaxY;
    int observedMax = kGenMinY;

    for (int localX = 0; localX < engine::shared::CHUNK_SIZE; ++localX) {
        const int worldX = col.x * engine::shared::CHUNK_SIZE + localX;
        for (int localZ = 0; localZ < engine::shared::CHUNK_SIZE; ++localZ) {
            const int worldZ = col.z * engine::shared::CHUNK_SIZE + localZ;

            // 高度场采样：世界坐标未缩放，seed 显式传入（流式重建命脉）。
            // 句柄内部是"DLL 函数指针 + 实例指针"，一次间接跳转，与虚调用同量级。
            const double n = height_.sample(
                static_cast<double>(worldX), static_cast<double>(worldZ), preset_.seed);
            int surfaceY = surfaceBase_ +
                static_cast<int>(std::lround(n * static_cast<double>(amplitude_)));
            surfaceY = std::clamp(surfaceY, kGenMinY, surfaceMax_);
            observedMin = std::min(observedMin, surfaceY);
            observedMax = std::max(observedMax, surfaceY);

            for (int y = kGenMinY; y <= surfaceY; ++y) {
                const int depth = surfaceY - y;   // 相对地表深度：0 = 地表

                // 查地层（线性扫；层数少，阶段 1 不提前优化——触发器模式）
                std::uint16_t type = 0;
                bool found = false;
                for (const Layer& l : layers_) {
                    if (depth >= l.depthMin && depth <= l.depthMax) {
                        type = l.type;
                        found = true;
                        break;
                    }
                }
                if (!found) continue;   // 连续性已校验，理论上不可达；兜底跳过

                // y ∈ [0,63] → 层号 0..3；写入按块内坐标 &15 定位
                out.chunks[y >> 4].set(worldX, y, worldZ,
                                       static_cast<engine::shared::PaletteEntry>(type));
                ++out.blockCount;
            }
        }
    }

    out.minSurfaceY = observedMin;
    out.maxSurfaceY = observedMax;
}

void ColumnGenerator::releaseNoise() {
    height_.reset();   // 调 DLL 里的 destroy —— 调用时刻 DLL 必须仍然加载着
    prepared_ = false;
}

} // namespace engine::worldgen
