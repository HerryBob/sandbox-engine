#pragma once
// 纹理图集（texture atlas）：一张 128² 大纹理装所有方块小纹理（每格 16²）。
// 职责：① 从注册表收集纹理名 → 加载 textures/<name>.png → 拼进图集
//       ② 名字 → 格（row,col）→ UV 区间（供 meshing 填顶点 UV）
// 渲染时只绑定一张大纹理，每面 UV 指向自己那一格 → 免频繁切换绑定。
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

namespace engine::render {

// 图集常量：格 16² × 网格 8×8 → 128² 图集（64 格容量）
constexpr std::uint32_t kAtlasTile = 16;   // 每格边长（像素）
constexpr std::uint32_t kAtlasGrid = 8;    // 每边格数
constexpr std::uint32_t kAtlasSize = kAtlasTile * kAtlasGrid; // 128

// 格位置：row（行）/ col（列）
struct AtlasSlot {
    std::uint8_t row = 0;
    std::uint8_t col = 0;
};

class TextureAtlas {
public:
    // 遍历注册表：收集所有 BlockInfo::texture → 加载 → 填格（空槽 = 棋盘格）
    bool buildFromRegistry();

    // 名字 → 格（未注册返回 false）
    bool slotOf(std::string_view name, AtlasSlot& out) const;

    // 128² RGBA 像素（4 通道，供 GPU 上传）
    const std::vector<std::uint8_t>& rgba() const { return pixels_; }

    // 格 → UV 矩形 {u0, v0, u1, v1}（归一化 0~1，meshing 填顶点用）
    static glm::vec4 slotUV(AtlasSlot s);

    // 缺失纹理兜底：第 0 格恒为 default 棋盘格（未注册纹理的方块显示它）
    static glm::vec4 fallbackUV() { return slotUV(AtlasSlot{0, 0}); }

private:
    // 加载 textures/<name>.png → NearestNeighbor 缩 16² → 写入格（失败则格留棋盘格）
    bool addFromFile(std::string_view name, AtlasSlot slot);

    std::vector<std::uint8_t> pixels_;                         // 128² × 4 通道
    std::unordered_map<std::string, AtlasSlot> slots_;         // 纹理名 → 格
};

} // namespace engine::render
