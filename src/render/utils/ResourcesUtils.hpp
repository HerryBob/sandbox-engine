#pragma once
#include <cstdint>
// 资源创建族：深度格式选择、内存类型查找、图像/缓冲创建（均绑定显存）。
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace engine::render::util {

// 深度格式选择：D32（纯深度）→ D24+8（模板兼容），都不支持返回 UNDEFINED。
VkFormat pickDepthFormat(VkPhysicalDevice dev);
const char* depthFormatName(VkFormat f);

// 在 typeFilter 位掩码命中的内存类型里，找第一个满足 props 的（如 HOST_VISIBLE 或 DEVICE_LOCAL）。
uint32_t findMemoryType(VkPhysicalDevice dev, uint32_t typeFilter, VkMemoryPropertyFlags props);

// 通用图像创建：参数化 尺寸/格式/用途/mip 级数/采样次数（MSAA 复用），绑定显存。
bool createImage(VkDevice dev, VkPhysicalDevice pdev, uint32_t w, uint32_t h, VkFormat format,
    VkImageUsageFlags usage, uint32_t mipLevels, VkSampleCountFlagBits samples,
    VkImage& image, VkDeviceMemory& imageMemory);

// 通用缓冲创建：参数化 大小/用途/内存属性，绑定显存。
bool createBuffer(VkDevice dev, VkPhysicalDevice pdev, VkDeviceSize size,
    VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
    VkBuffer& buf, VkDeviceMemory& mem);

// ---- mipmap（通用）----
// 完整 mip 级数：floor(log2(max(w,h))) + 1（1×1 为止）。w/h 任一为 0 时返回 1。
std::uint32_t mipLevelsFor(std::uint32_t width, std::uint32_t height);

// 生成 mip 链：vkCmdBlitImage 逐级减半（每级先转 TRANSFER_DST、生成后转 TRANSFER_SRC）。
// 前提：image 已带 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT，
//       且**第 0 级已经写完**；调用方负责在调用前把 image 置为 TRANSFER_DST_OPTIMAL 之外
//       的"当前布局任意"，本函数结束时 image 处于 SHADER_READ_ONLY_OPTIMAL。
// 为什么用 blit 而不是 compute/手写盒式滤波：格式自带 sRGB 解码 + 线性过滤，
// 一行调用就能覆盖所有格式，且这是广泛验证过的做法。
bool generateMipmaps(VkDevice dev, VkPhysicalDevice pdev, VkCommandPool cmdPool,
                     VkQueue queue, VkImage image, VkFormat format,
                     std::uint32_t width, std::uint32_t height, std::uint32_t mipLevels);

// 通用图像视图创建：参数化 图像/格式/方面（COLOR/DEPTH），固定 2D、全 mip、全层。
bool createImageView(VkDevice dev, VkImage image, VkFormat format,
    VkImageAspectFlags aspect, VkImageView& view, std::uint32_t mipLevels = 1);

} // namespace engine::render::util
