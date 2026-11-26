#include "core/Log.hpp"

#include <sstream>
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
    VkImageAspectFlags aspect, VkImageView& view, std::uint32_t mipLevels) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange.aspectMask = aspect;
    info.subresourceRange.baseMipLevel = 0;
    info.subresourceRange.levelCount = mipLevels;   // mip 链要全范围，否则采样器只能看到第 0 级
    info.subresourceRange.baseArrayLayer = 0;
    info.subresourceRange.layerCount = 1;
    return vkCreateImageView(dev, &info, nullptr, &view) == VK_SUCCESS;
}


// ============================================================================
// mipmap（通用实现）
// ============================================================================

std::uint32_t mipLevelsFor(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) return 1;
    std::uint32_t levels = 1;
    std::uint32_t m = (width > height) ? width : height;
    while (m > 1) { m >>= 1; ++levels; }
    return levels;
}

namespace {

bool beginOneShot(VkDevice dev, VkCommandPool pool, VkCommandBuffer& cmd) {
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &ai, &cmd) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS;
}

bool endOneShot(VkDevice dev, VkCommandPool pool, VkQueue queue, VkCommandBuffer cmd) {
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) return false;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return false;
    vkQueueWaitIdle(queue);   // 只在纹理装载期调用，一次性开销
    vkFreeCommandBuffers(dev, pool, 1, &cmd);
    return true;
}

void barrierMip(VkCommandBuffer cmd, VkImage image, std::uint32_t level,
                VkImageLayout oldLayout, VkImageLayout newLayout,
                VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = level;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace

bool generateMipmaps(VkDevice dev, VkPhysicalDevice pdev, VkCommandPool cmdPool,
                     VkQueue queue, VkImage image, VkFormat format,
                     std::uint32_t width, std::uint32_t height, std::uint32_t mipLevels) {
    if (mipLevels <= 1) return true;   // 单级：没有链可生成

    // ① 格式必须支持线性过滤的 blit（体素图集/实体贴图都是 RGBA8_SRGB，满足）
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(pdev, format, &props);
    if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        ENGINE_LOG_WARN("Vulkan", "该格式不支持线性过滤 blit，跳过 mipmap 生成");
        return false;
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!beginOneShot(dev, cmdPool, cmd)) return false;

    // ② 第 0 级：TRANSFER_DST → TRANSFER_SRC（它已经由 copyBufferToImage 填好）
    barrierMip(cmd, image, 0,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);

    // ③ 逐级减半：src=level-1（TRANSFER_SRC）→ dst=level（TRANSFER_DST）→ 再转回 SRC
    std::int32_t mipW = static_cast<std::int32_t>(width);
    std::int32_t mipH = static_cast<std::int32_t>(height);
    for (std::uint32_t level = 1; level < mipLevels; ++level) {
        barrierMip(cmd, image, level,
                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   0, VK_ACCESS_TRANSFER_WRITE_BIT);

        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mipW, mipH, 1};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = level - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {(mipW > 1) ? mipW / 2 : 1, (mipH > 1) ? mipH / 2 : 1, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = level;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;
        vkCmdBlitImage(cmd,
            image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);

        barrierMip(cmd, image, level,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);

        if (mipW > 1) mipW /= 2;
        if (mipH > 1) mipH /= 2;
    }

    // ④ 全链转成 SHADER_READ_ONLY（采样器只读）
    VkImageMemoryBarrier toRead{};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.image = image;
    toRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toRead.subresourceRange.baseMipLevel = 0;
    toRead.subresourceRange.levelCount = mipLevels;
    toRead.subresourceRange.baseArrayLayer = 0;
    toRead.subresourceRange.layerCount = 1;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toRead);

    const bool ok = endOneShot(dev, cmdPool, queue, cmd);
    if (ok) {
        std::ostringstream os;
        os << "mipmap 链已生成：" << width << "x" << height << " → " << mipLevels << " 级";
        ENGINE_LOG_INFO("Vulkan", os.str());
    }
    return ok;
}
} // namespace engine::render::util
