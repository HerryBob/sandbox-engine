#pragma once

#include "worldgen/noise/NoiseHandle.hpp"
#include "worldgen/preset/WorldPreset.hpp"
#include "worldgen/streaming/GenTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::worldgen {

// 列生成器：纯计算，不持有 World、不碰方块存储、不加锁。
// 后台生成线程只持有本对象的 const 引用 —— 这是"后台线程永不持有 World"边界的落点。
//
// 契约：同 preset（同 seed + 同噪声参数）= 同产物；generateColumn 是 const、无隐藏状态。
// 生命周期：prepare() 必须在使用前调用一次（解析地层表 + 创建高度场句柄）；
//           之后只读，可跨线程并发调用 generateColumn。
// 所有权：height_ 归本对象所有，析构时调 DLL 的 destroy（谁创建谁销毁）。
//         因此使用方只应持有本对象的引用，而不是拷贝它。
class ColumnGenerator {
public:

    // 按值持有预设：预设是可拷贝的纯数据；句柄的生命周期绑定本对象，
    // 不与调用方的 preset 变量共享（prepare 失败/重复调用时语义清晰）。
    explicit ColumnGenerator(WorldPreset preset);

    // 预设校验 + 地层表解析 + 高度场句柄创建（一次）；失败返回 false（stderr 已说明原因）。
    bool prepare();
    bool prepared() const { return prepared_; }

    // 生成一列（16×16 方块列 × y ∈ [kGenMinY, kGenMaxY]）→ 4 个 16³ ChunkData。
    // out 会被整体重置（复用同一对象时不会残留上一列数据）。
    // 必须已 prepare()；未 prepare 时产出空列（调用方应先检查 prepared()）。
    void generateColumn(const ColPos& col, ColumnBlocks& out) const;

    // 提前释放高度场句柄（不销毁本对象）：必须在卸载 provider 所在 DLL 之前调用。
    // 句柄里存的 destroy 是 DLL 内的函数指针，DLL 被 FreeLibrary 后就成了悬空指针；
    // 顺序错了不会在正常路径报错，只会在进程退出时静默访问已释放的映像（UB）。
    void releaseNoise();

private:
    // 地层：方块类型 + 相对地表深度区间（0 = 地表）。
    struct Layer {
        std::uint16_t type = 0;  // BlockType 值
        int depthMin = 0;
        int depthMax = 0;
    };

    WorldPreset preset_;          // 值持有（纯数据，可拷贝）
    NoiseHandle height_;          // prepare 时按 preset 参数创建（实例在 DLL 内）
    std::vector<Layer> layers_;   // prepare 结果：按 depthMin 升序、无断档
    int surfaceBase_ = 0;         // lround(baseHeight)
    int amplitude_ = 0;           // lround(amplitude)
    int surfaceMax_ = 0;          // 地表可用上限 = min(kGenMaxY, 地层最大深度)
    bool prepared_ = false;
};

} // namespace engine::worldgen
