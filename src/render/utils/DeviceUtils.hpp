#pragma once
// 实例/设备探测：图层扩展枚举、显卡枚举、队列族选择、命名。
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vector>

namespace engine::render::util {

constexpr const char* kRequiredInstanceExtensions[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_WIN32_SURFACE_EXTENSION_NAME, // surface 扩展提前启用：swapchain 直接可用
};
constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

// Vulkan 不保证驱动带全量扩展/图层——必须先枚举再启用，硬塞会创建失败。
bool hasInstanceLayer(const char* name);
bool hasInstanceExtension(const char* name);

// 枚举本机全部物理设备（显卡），pickDevice 用。
std::vector<VkPhysicalDevice> enumeratePhysicalDevices(VkInstance instance);

// 某队列族的总队列数（日志用，如"容量 8"）。
uint32_t queueFamilyQueueCount(VkPhysicalDevice dev, uint32_t familyIndex);

// 设备类型枚举 → 中文名（日志用）。
const char* deviceTypeName(VkPhysicalDeviceType t);

// 找一个"同时支持图形 + 呈现"的队列族（消费级 GPU 通常一个族全包）。
uint32_t findGraphicsPresentFamily(VkPhysicalDevice dev, VkSurfaceKHR surface);

//查询设备支持的最大采样数（颜色/深度交集，从高到低试探）
VkSampleCountFlagBits getMaxUsableSampleCount(VkPhysicalDevice dev);
} // namespace engine::render::util
