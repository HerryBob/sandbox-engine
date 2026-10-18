#include "ResourcesUtils.hpp"

#include <cstddef>
namespace engine::render::util {

// 深度格式选择：D32 优先，D24+8 兜底
VkFormat pickDepthFormat(VkPhysicalDevice dev) {
    const VkFormat candidates[] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT };
    for (VkFormat f : candidates) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(dev, f, &p);
        if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return f;
    }
    return VK_FORMAT_UNDEFINED; // 调用方判错
}

// 深度格式 → 名字（日志用）
const char* depthFormatName(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D32_SFLOAT:        return "D32_SFLOAT";
    case VK_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8";
    default:                          return "?";
    }
}

// 在候选内存类型里找第一个满足属性要求的
uint32_t findMemoryType(VkPhysicalDevice dev, uint32_t typeFilter,
    VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(dev, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((typeFilter & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return UINT32_MAX;
}

// 通用图像创建：①订单 → ②空壳 → ③问地 → ④选地 → ⑤买地 → ⑥盖房（绑定显存）
bool createImage(VkDevice dev, VkPhysicalDevice pdev,
    uint32_t width, uint32_t height, VkFormat format,
    VkImageUsageFlags usage,
    uint32_t mipLevels, VkSampleCountFlagBits samples,
    VkImage& image, VkDeviceMemory& imageMemory) {
    // ① 填图像"订单"（规格：尺寸/格式/维度/用途/采样）
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = { width, height, 1 };
    imageInfo.mipLevels = mipLevels;                    // ← 多级纹理参数（现在传 1）
    imageInfo.arrayLayers = 1;
    imageInfo.samples = samples;                        // ← 采样次数（现在传 1x，MSAA 时复用）
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;         // GPU 最优排布
    imageInfo.usage = usage;                            // ← 用途由调用方定
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // ② 创建图像对象（空壳：登记了规格，但此刻还没有实际存储）
    if (vkCreateImage(dev, &imageInfo, nullptr, &image) != VK_SUCCESS) return false;

    // ③ 问驱动：这块图像需要多大的地（字节数）、什么类型的地（memoryTypeBits）
    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(dev, image, &memReq);
    // ④ 在可用内存类型里选"显存"（DEVICE_LOCAL：GPU 最快访问）
    uint32_t idx = findMemoryType(pdev, memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); // 显存
    if (idx == UINT32_MAX) return false;
    // ⑤ 买地：按 size 分配真显存（返回 VkDeviceMemory 句柄）
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = idx;
    if (vkAllocateMemory(dev, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) return false;
    // ⑥ 盖房：把空壳绑定到这块显存（offset 0 = 从地开头用起）
    vkBindImageMemory(dev, image, imageMemory, 0);
    return true;
}

// 通用缓冲创建，绑定显存
bool createBuffer(VkDevice dev, VkPhysicalDevice pdev, VkDeviceSize size,
    VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
    VkBuffer& buf, VkDeviceMemory& mem) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &bufferInfo, nullptr, &buf) != VK_SUCCESS) return false;

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(dev, buf, &memReq);
    uint32_t idx = findMemoryType(pdev, memReq.memoryTypeBits, props);
    if (idx == UINT32_MAX) return false;
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = idx;
    if (vkAllocateMemory(dev, &allocInfo, nullptr, &mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(dev, buf, mem, 0);
    return true;
}

// 通用图像视图创建：固定样板收起（2D、mip0 全级、层0 全层），只露差异参数。
bool createImageView(VkDevice dev, VkImage image, VkFormat format,
    VkImageAspectFlags aspect, VkImageView& view) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange.aspectMask = aspect;
    info.subresourceRange.baseMipLevel = 0;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.baseArrayLayer = 0;
    info.subresourceRange.layerCount = 1;
    return vkCreateImageView(dev, &info, nullptr, &view) == VK_SUCCESS;
}

} // namespace engine::render::util
