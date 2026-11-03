#pragma once

#include "worldgen/preset/WorldPreset.hpp"
#include "worldgen/streaming/GenTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::worldgen {

// 列生成器（1b 阶段 1）：纯计算，不持有 World、不碰方块存储、不加锁。
// 后台生成线程只持有本对象的 const 引用 —— 这是"后台线程永不持有 World"边界的落点。
//
// 契约：同 preset（同 seed + 同噪声参数）= 同产物；generateColumn 是 const、无隐藏状态。
// 生命周期：prepare() 必须在使用前调用一次（解析地层表 + 预设校验）；之后只读，
//           可跨线程并发调用 generateColumn（layers_ 只读，无共享可变状态）。
//
// 与 1a 的关系：本类是 1a WorldGenerator 逐列计算部分的可复用化，
//              阶段 1 只新增、不替换，旧 generate() 保持原样以便基线回归。
class ColumnGenerator {
public:
    // 聚合统计（一次生成会话的诊断口径；由调用方逐列累加）
    struct Stats {
        std::size_t columns = 0;    // 已生成列数（chunk 列）
        std::size_t blocks = 0;     // 已写入方块数
        int minSurfaceY = 0;        // 地表高度范围
        int maxSurfaceY = 0;
    };

    explicit ColumnGenerator(const WorldPreset& preset);

    // 预设校验 + 地层表解析（一次）；失败返回 false（stderr 已说明原因）。
    bool prepare();
    bool prepared() const { return prepared_; }

    // 生成一列（16×16 方块列 × y ∈ [kGenMinY, kGenMaxY]）→ 4 个 16³ ChunkData。
    // out 会被整体重置（复用同一对象时不会残留上一列数据）。
    // 必须已 prepare()；未 prepare 时产出空列（调用方应先检查 prepared()）。
    void generateColumn(const ColPos& col, ColumnBlocks& out) const;

private:
    // 地层：方块类型 + 相对地表深度区间（0 = 地表）。
    struct Layer {
        std::uint8_t type = 0;   // BlockType 值
        int depthMin = 0;
        int depthMax = 0;
    };

    const WorldPreset& preset_;   // 只读引用（preset 生命周期须覆盖生成会话）
    std::vector<Layer> layers_;   // prepare 结果：按 depthMin 升序、无断档
    int surfaceBase_ = 0;         // lround(baseHeight)
    int amplitude_ = 0;           // lround(amplitude)
    int surfaceMax_ = 0;          // 地表可用上限 = min(kGenMaxY, 地层最大深度)
    bool prepared_ = false;
};

} // namespace engine::worldgen
