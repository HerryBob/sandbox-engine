#pragma once
// 纹理图集（texture atlas）：一张 128² 大纹理装所有方块小纹理（每格 16²）。
// 职责：① 从方块注册表 + 模型注册表收集纹理名 → 按索引查 textures/block/<name>.png → 拼进图集
//       ② 名字 → 格（row,col）→ UV 区间（供 meshing 填顶点 UV）
// 渲染时只绑定一张大纹理，每面 UV 指向自己那一格 → 免频繁切换绑定。
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

namespace engine::assets {

// 图集常量：格 16²，每边格数与图集边长由声明条目数在运行期算出
constexpr std::uint32_t kAtlasTile = 16;   // 每格边长（像素）
// 通道数：RGBA 四通道 × 1 字节/通道 = 每像素 4 字节（PNG 解码 / Vulkan 导入均按此）
constexpr std::uint32_t kAtlasChannels = 4;

// 格位置：row（行）/ col（列）
struct AtlasSlot {
    std::uint8_t row = 0;
    std::uint8_t col = 0;
};

class TextureAtlas {
public:
    // 读 assets/atlases/blocks.json 的 textures 数组逐格铺图（第 0 格恒 default.png）。
    // 声明缺失时回退为「遍历注册表收集纹理名」。每边格数按条目数取 sqrt 上整。
    bool build();

    // 名字 → 格（未注册返回 false）
    bool slotOf(std::string_view name, AtlasSlot& out) const;

    // 像素（size × size RGBA，供 GPU 上传）
    const std::vector<std::uint8_t>& rgba() const { return pixels_; }

    // 每边格数 / 像素边长（= kAtlasTile × grid）
    std::uint32_t grid() const { return grid_; }
    std::uint32_t size() const { return size_; }

    // 格 → UV 矩形 {u0, v0, u1, v1}（归一化 0~1，meshing 填顶点用）
    glm::vec4 slotUV(AtlasSlot s) const;

    // 缺失纹理兜底：第 0 格恒为 default（default.png，未注册纹理的方块显示它）
    glm::vec4 fallbackUV() const { return slotUV(AtlasSlot{0, 0}); }

private:
    // 按索引查 textures/block/<name>.png → 直拷入格（失败则格留空，不崩引擎）
    bool addFromFile(std::string_view name, AtlasSlot slot);
    std::vector<std::uint8_t> pixels_;                         // size² × 4 通道
    std::unordered_map<std::string, AtlasSlot> slots_;         // 纹理名 → 格
    std::uint32_t grid_ = 1;                                   // 每边格数
    std::uint32_t size_ = kAtlasTile;                           // 像素边长
};

} // namespace engine::assets
