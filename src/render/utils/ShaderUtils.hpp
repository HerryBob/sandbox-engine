#pragma once
// 着色器：读预编译 .spv 文件 → VkShaderModule。
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace engine::render::util {

// 读预编译 .spv 文件 → VkShaderModule（管线创建后即可销毁，GPU 已复制）。
VkShaderModule loadShaderModule(VkDevice dev, const char* spvPath);

} // namespace engine::render::util
