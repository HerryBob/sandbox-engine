#pragma once
// 纹理图集（texture atlas）：一张 N² 大纹理装所有方块小纹理（每格 16²）。
// 职责：① 从方块注册表 + 模型注册表收集纹理名 → 按索引查 textures/block/<name>.png → 拼进图集
//       ② 名字 → 格（row,col）→ UV 区间（供 meshing 填顶点 UV）
// 格序由引擎自动决定：先按方块 id 升序取注册表纹理名，再补模型表里没被方块引用的槽位，
// 同名只占一格；第 0 格恒为 default.png（缺失纹理兜底）。
// 渲染时只绑定一张大纹理，每面 UV 指向自己那一格 → 免频繁切换绑定。
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

namespace engine::assets {

// 图集常量：格 16²，每边格数与图集边长由纹理条目数在运行期算出
constexpr std::uint32_t kAtlasTile = 16;   // 每格边长（像素）
// 通道数：RGBA 四通道 × 1 字节/通道 = 每像素 4 字节（PNG 解码 / Vulkan 导入均按此）
constexpr std::uint32_t kAtlasChannels = 4;
// 每格四周的**间隔**（像素）：格子之间留 1px，并把边缘像素挤进这 1px。
// 为什么需要：格子紧挨着时，采样点落在边界会取到隔壁格（mipmap 线性缩小更是必然跨格），
// 隔壁若是 default.png（品红缺失纹理）就会在方块边缘冒出紫线。
// 留 1px + 边缘挤出后，边界上的采样取到的永远是**本格边缘的颜色**。
constexpr std::uint32_t kAtlasPadding = 1;
// 格距（格 + 两侧间隔）。像素边长 = pitch × grid。
constexpr std::uint32_t kAtlasPitch = kAtlasTile + 2 * kAtlasPadding;

// 格位置：row（行）/ col（列）
struct AtlasSlot {
    std::uint8_t row = 0;
    std::uint8_t col = 0;
};

// 纯数学：图集某格的 UV 矩形（含半像素内缩）。
// 抽成自由函数是为了可单测 —— 这条 UV 边界过去出过"方块边缘冒紫色短线"的 bug
// （UV 正好落在格边界 → 最近邻采样取到隔壁格 default.png 的品红）。
// grid = 每边格数；size = 图集像素边长。
// grid/size：图集参数；tile/pad：格边长与四周间隔（像素）。
// 返回值是**格内容**（不含间隔）的 UV 矩形。
glm::vec4 atlasSlotUVPure(std::uint32_t grid, std::uint32_t size,
                          std::uint32_t tile, std::uint32_t pad,
                          std::uint32_t col, std::uint32_t row);

class TextureAtlas {
public:
    // 自动收集纹理名并逐格铺图（第 0 格恒 default.png）。每边格数按条目数取 sqrt 上整。
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
    // ---- build() 的四段（规则1 拆分）----
    std::vector<std::string> collectUsedTextures() const;   // ① 方块 + 模型引用的纹理名（顺序确定）
    void layoutGrid(std::size_t count);                     // ② 每边格数 + 像素缓冲
    bool placeDefaultSlot();                                // ③ 第 0 格 = default.png

    // ④ 铺格结果（供汇总日志）
    struct Placement {
        std::size_t loaded = 0;     // 成功进格的张数
        std::size_t failed = 0;     // 读取/尺寸失败的张数
        std::size_t occupied = 1;   // 已占用格数（含第 0 格）
    };
    Placement placeSlots(const std::vector<std::string>& names);

    // 按索引查 textures/block/<name>.png → 直拷入格（失败则格留空，不崩引擎）
    bool addFromFile(std::string_view name, AtlasSlot slot);
    std::vector<std::uint8_t> pixels_;                         // size² × 4 通道
    std::unordered_map<std::string, AtlasSlot> slots_;         // 纹理名 → 格
    std::uint32_t grid_ = 1;                                   // 每边格数
    std::uint32_t size_ = kAtlasTile;                           // 像素边长
};

} // namespace engine::assets
