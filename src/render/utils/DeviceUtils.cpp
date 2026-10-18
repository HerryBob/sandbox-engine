#include "DeviceUtils.hpp"
#include "core/Log.hpp"

#include <cstring>
#include <vector>

namespace engine::render::util {

// 枚举实例层，确认校验层是否可用
bool hasInstanceLayer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const auto& l : layers)
        if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}

// 枚举实例扩展，确认是否已启用
bool hasInstanceExtension(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
    for (const auto& e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

// 枚举本机全部物理设备（显卡）
std::vector<VkPhysicalDevice> enumeratePhysicalDevices(VkInstance instance) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    return devices;
}

// 某队列族的总队列数（日志用）
uint32_t queueFamilyQueueCount(VkPhysicalDevice dev, uint32_t familyIndex) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
    std::vector<VkQueueFamilyProperties> fams(count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, fams.data());
    return (familyIndex < fams.size()) ? fams[familyIndex].queueCount : 0;
}

// 设备类型枚举 → 中文名（日志用）
const char* deviceTypeName(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "独立显卡";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "集成显卡";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "虚拟 GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU 软渲染";
    default:                                     return "未知";
    }
}

// 找一个同时支持图形+呈现的队列族
uint32_t findGraphicsPresentFamily(VkPhysicalDevice dev, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
    std::vector<VkQueueFamilyProperties> fams(count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, fams.data());
    for (uint32_t i = 0; i < count; ++i) {
        if (!(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &supported);
        if (supported == VK_TRUE) return i;
    }
    return UINT32_MAX;
}
//查询设备支持的mass采样数
VkSampleCountFlagBits getMaxUsableSampleCount(VkPhysicalDevice dev) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(dev, &props);
    VkSampleCountFlags counts =
        props.limits.framebufferColorSampleCounts &    // 颜色附件最多支持几倍采样
        props.limits.framebufferDepthSampleCounts;     // 深度也要支持（取交集）
    if (counts & VK_SAMPLE_COUNT_64_BIT) return VK_SAMPLE_COUNT_64_BIT;  // 从高到低试
    if (counts & VK_SAMPLE_COUNT_32_BIT) return VK_SAMPLE_COUNT_32_BIT;
    if (counts & VK_SAMPLE_COUNT_16_BIT) return VK_SAMPLE_COUNT_16_BIT;
    if (counts & VK_SAMPLE_COUNT_8_BIT)  return VK_SAMPLE_COUNT_8_BIT;
    if (counts & VK_SAMPLE_COUNT_4_BIT)  return VK_SAMPLE_COUNT_4_BIT;
    if (counts & VK_SAMPLE_COUNT_2_BIT)  return VK_SAMPLE_COUNT_2_BIT;
    return VK_SAMPLE_COUNT_1_BIT;                             // 最低保底：无抗锯齿

}

} // namespace engine::render::util
