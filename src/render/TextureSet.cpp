#include "render/TextureSet.hpp"

#include <cstring>

#include "core/Log.hpp"
#include "render/utils/CommandUtils.hpp"
#include "render/utils/ResourcesUtils.hpp"

namespace engine::render {

bool TextureSet::init(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue graphicsQueue,
                      VkCommandPool cmdPool, const void* rgba, std::uint32_t width,
                      std::uint32_t height) {
    physicalDevice_ = physicalDevice;
    device_         = device;
    graphicsQueue_  = graphicsQueue;
    cmdPool_        = cmdPool;
    if (!createImage(rgba, width, height)) return false;
    if (!createImageView()) return false;
    if (!createSampler()) return false;
    return true;
}

bool TextureSet::createImage(const void* rgba, std::uint32_t width, std::uint32_t height) {
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 4;   // RGBA8

    // mipmap：完整链（blit 需要 TRANSFER_SRC，生成 mip 时要读第 N-1 级）
    mipLevels_ = util::mipLevelsFor(width, height);
    if (!util::createImage(device_, physicalDevice_, width, height, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            mipLevels_, VK_SAMPLE_COUNT_1_BIT, image_, imageMemory_)) {
        ENGINE_LOG_ERROR("Vulkan", "纹理图像创建失败");
        return false;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    if (!util::createBuffer(device_, physicalDevice_, imageSize,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging, stagingMem)) {
        ENGINE_LOG_ERROR("Vulkan", "纹理 staging buffer 创建失败");
        return false;
    }
    void* data = nullptr;
    vkMapMemory(device_, stagingMem, 0, imageSize, 0, &data);
    std::memcpy(data, rgba, static_cast<std::size_t>(imageSize));
    vkUnmapMemory(device_, stagingMem);

    util::transitionImageLayout(device_, graphicsQueue_, cmdPool_, image_,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    util::copyBufferToImage(device_, graphicsQueue_, cmdPool_, staging, image_, width, height);
    // 生成 mip 链：入口布局必须是 TRANSFER_DST_OPTIMAL（刚拷贝完就是这样），
    // 出口由 generateMipmaps 转成 SHADER_READ_ONLY_OPTIMAL，所以这里不再手工 transition。
    if (!util::generateMipmaps(device_, physicalDevice_, cmdPool_, graphicsQueue_,
            image_, VK_FORMAT_R8G8B8A8_SRGB, width, height, mipLevels_)) {
        ENGINE_LOG_WARN("Vulkan", "图集 mipmap 生成失败，退回单级采样");
        util::transitionImageLayout(device_, graphicsQueue_, cmdPool_, image_,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        mipLevels_ = 1;
    }

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);
    return true;
}

bool TextureSet::createImageView() {
    if (!util::createImageView(device_, image_, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_ASPECT_COLOR_BIT, view_, mipLevels_)) {
        ENGINE_LOG_ERROR("Vulkan", "纹理视图创建失败");
        return false;
    }
    return true;
}

bool TextureSet::createSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;      // 放大最近邻：像素风硬块（线性会糊）
    // 缩小用线性 + mip 链：远处方块不再闪烁（摩尔纹），近处仍是硬边
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;  // 图集防相邻格泄漏
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    // 每格四周有 1px 间隔且**边缘像素已挤出**（见 TextureAtlas::addFromFile），
    // 所以跨格渗色问题不存在了 → 放开到完整 mip 链（远处更稳、更省带宽）。
    samplerInfo.maxLod = static_cast<float>(mipLevels_ > 0 ? mipLevels_ - 1 : 0);
    if (vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "纹理采样器创建失败");
        return false;
    }
    return true;
}

void TextureSet::shutdown() {
    if (sampler_) { vkDestroySampler(device_, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
    if (view_) { vkDestroyImageView(device_, view_, nullptr); view_ = VK_NULL_HANDLE; }
    if (image_) { vkDestroyImage(device_, image_, nullptr); image_ = VK_NULL_HANDLE; }
    if (imageMemory_) { vkFreeMemory(device_, imageMemory_, nullptr); imageMemory_ = VK_NULL_HANDLE; }
}

} // namespace engine::render
