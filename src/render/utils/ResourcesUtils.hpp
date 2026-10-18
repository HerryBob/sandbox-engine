#pragma once
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

// 通用图像视图创建：参数化 图像/格式/方面（COLOR/DEPTH），固定 2D、全 mip、全层。
bool createImageView(VkDevice dev, VkImage image, VkFormat format,
    VkImageAspectFlags aspect, VkImageView& view);

} // namespace engine::render::util
