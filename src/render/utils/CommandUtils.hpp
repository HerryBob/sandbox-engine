#pragma once
// 命令操作序列：一次性命令缓冲、staging 搬运、布局转换、map 上传。
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace engine::render::util {

// 一次性命令缓冲：录一段"创建期临时活"（搬运/转换），提交后即弃。
VkCommandBuffer beginSingleTimeCommands(VkDevice dev, VkCommandPool pool);
void endSingleTimeCommands(VkDevice dev, VkQueue queue, VkCommandPool pool, VkCommandBuffer cmd);

// staging 缓冲 → 图像（纹理像素搬进显存，目标须已转 TRANSFER_DST_OPTIMAL）。
void copyBufferToImage(VkDevice dev, VkQueue queue, VkCommandPool pool,
    VkBuffer src, VkImage dst, uint32_t w, uint32_t h);

// 图像布局转换（UNDEFINED→TRANSFER_DST、TRANSFER_DST→SHADER_READ_ONLY）。
void transitionImageLayout(VkDevice dev, VkQueue queue, VkCommandPool pool,
    VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout);

// map 显存 → memcpy 写入 → unmap（staging 上传的通用三连）。
void mapAndCopy(VkDevice dev, VkDeviceMemory mem, VkDeviceSize size, const void* src);

} // namespace engine::render::util
