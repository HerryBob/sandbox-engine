#include "SwapchainUtils.hpp"

#include <vector>

namespace engine::render::util {

// 查询 surface 能力（画布数量范围/尺寸/旋转）
VkSurfaceCapabilitiesKHR querySurfaceCapabilities(VkPhysicalDevice dev, VkSurfaceKHR surface) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev, surface, &caps);
    return caps;
}

// 查询 surface 支持的颜色格式，挑 B8G8R8A8_SRGB（颜色正确），没有则取第一个
VkSurfaceFormatKHR chooseSwapSurfaceFormat(VkPhysicalDevice dev, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &count, fmts.data());
    for (const auto& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return f;
    }
    return fmts[0]; // 兜底：第一个
}

// 查询呈现模式，挑 MAILBOX（低延迟无撕裂），没有则 FIFO（垂直同步）
VkPresentModeKHR chooseSwapPresentMode(VkPhysicalDevice dev, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface, &count, modes.data());
    for (auto m : modes)
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
    return VK_PRESENT_MODE_FIFO_KHR; // 兜底：垂直同步
}

// 计算画布尺寸：驱动固定值优先；UINT32_MAX 表示尺寸随窗口变，则问窗口
VkExtent2D chooseSwapExtent(VkPhysicalDevice dev, VkSurfaceKHR surface, GLFWwindow* window) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev, surface, &caps);
    if (caps.currentExtent.width != UINT32_MAX)
        return caps.currentExtent;
    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    return { static_cast<uint32_t>(w), static_cast<uint32_t>(h) };
}

} // namespace engine::render::util
