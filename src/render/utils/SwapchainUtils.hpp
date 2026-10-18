#pragma once
// 交换链能力查询与择优（createSwapchain 用）：能力/格式/呈现模式/尺寸。
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace engine::render::util {

// 查询 surface 能力（画布数量范围/尺寸/旋转）。
VkSurfaceCapabilitiesKHR querySurfaceCapabilities(VkPhysicalDevice dev, VkSurfaceKHR surface);

// 查询 surface 支持的颜色格式，挑 B8G8R8A8_SRGB（颜色正确），没有则取第一个。
VkSurfaceFormatKHR chooseSwapSurfaceFormat(VkPhysicalDevice dev, VkSurfaceKHR surface);

// 查询呈现模式，挑 MAILBOX（低延迟无撕裂），没有则 FIFO（垂直同步）。
VkPresentModeKHR chooseSwapPresentMode(VkPhysicalDevice dev, VkSurfaceKHR surface);

// 计算画布尺寸：驱动固定值优先；UINT32_MAX 表示尺寸随窗口变，则问窗口。
VkExtent2D chooseSwapExtent(VkPhysicalDevice dev, VkSurfaceKHR surface, GLFWwindow* window);

} // namespace engine::render::util
