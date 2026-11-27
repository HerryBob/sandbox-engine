#pragma once
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

#include "render/hud/HudAtlas.hpp"

namespace engine::render::hud {

// ============================================================================
// CrosshairRenderer：屏幕中心的十字准星（2D 元素，与 F3 浮层**互不依赖**）
// ============================================================================
//
// 为什么单独一个类、单独一个文件：
//   · F3 浮层是"调试信息文本"，准星是"常驻 HUD 元素"，生命周期与开关都不同
//     （F3 可关，准星一直在；将来准星可能由模组换样式）；
//   · 准星将来要**外置为 DLL 模组** → 它只依赖 Vulkan 句柄 + HudAtlas（POD），
//     不引用 HudRenderer、不碰业务/物理对象；
//   · 同一目录（render/hud/）便于一起搬走，但不塞进同一个类里。
//
// 几何：经典十字（4 段短条 + 中心留空），顶点来自"图集第 0 格纯白块"，
// 所以和 F3 用同一份图集、同一种"覆盖率采样"着色方式 —— 不需要新贴图。
//
// 坐标：屏幕像素、原点左上、y 向下（与 shaders/hud.vert 完全一致）。
class CrosshairRenderer {
public:
    static constexpr std::size_t kMaxFramesInFlight = 2;
    static constexpr std::size_t kVertexCapacity = 48;   // (4 段主体 + 4 段描边) × 6 顶点

    CrosshairRenderer() = default;

    // 注意：inAtlas 由 HudRenderer 创建并持有，本类只借用，不释放。
    bool init(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue graphicsQueue,
              VkCommandPool cmdPool, VkRenderPass renderPass, VkExtent2D extent,
              VkSampleCountFlagBits msaaSamples, const HudAtlas& inAtlas);
    void shutdown();

    void setExtent(VkExtent2D extent) { extent_ = extent; }
    void setVisible(bool v) { visible_ = v; }
    bool visible() const { return visible_; }

    // 外观（像素）：臂长、条宽、中心留空、颜色（RGB 乘 alpha 混合）
    void setStyle(int armLength, int thickness, int gap,
                  float r, float g, float b, float a) {
        armLen_ = armLength; thickness_ = thickness; gap_ = gap;
        colorR_ = r; colorG_ = g; colorB_ = b; colorA_ = a;
    }

    // 画在 renderPass 的最后一趟（HUD 之后）；frameIndex 与在飞帧数一致。
    void draw(VkCommandBuffer cmd, uint32_t frameIndex);

private:
    bool createPipeline(VkRenderPass renderPass, VkSampleCountFlagBits msaaSamples);
    bool createDescriptor();
    bool createVertexBuffers();

    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice         device_ = VK_NULL_HANDLE;
    VkQueue          graphicsQueue_ = VK_NULL_HANDLE;
    VkCommandPool    cmdPool_ = VK_NULL_HANDLE;
    VkExtent2D       extent_{ 0, 0 };
    HudAtlas         atlas_{};

    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet       descriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            pipeline_ = VK_NULL_HANDLE;

    VkBuffer       vertexBuffer_[kMaxFramesInFlight]{};
    VkDeviceMemory vertexMemory_[kMaxFramesInFlight]{};
    void*          vertexMapped_[kMaxFramesInFlight]{};

    bool  visible_ = true;
    int   armLen_ = 8;          // 每段长度（像素）
    int   thickness_ = 2;       // 条宽（像素）
    int   gap_ = 2;             // 中心留空（像素）
    // 描边：白色准星压在明亮地面（草地/雪地）上会看不见，先画一层半透明暗色底
    float outlineA_ = 0.45f;
    float colorR_ = 1.0f, colorG_ = 1.0f, colorB_ = 1.0f, colorA_ = 0.85f;
};

} // namespace engine::render::hud
