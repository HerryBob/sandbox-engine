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

    if (!util::createImage(device_, physicalDevice_, width, height, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            1, VK_SAMPLE_COUNT_1_BIT, image_, imageMemory_)) {
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
    util::transitionImageLayout(device_, graphicsQueue_, cmdPool_, image_,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);
    return true;
}

bool TextureSet::createImageView() {
    if (!util::createImageView(device_, image_, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_ASPECT_COLOR_BIT, view_)) {
        ENGINE_LOG_ERROR("Vulkan", "纹理视图创建失败");
        return false;
    }
    return true;
}

bool TextureSet::createSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;      // 放大最近邻：16² 像素风硬块（线性会糊）
    samplerInfo.minFilter = VK_FILTER_NEAREST;      // 缩小最近邻
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;  // 图集防相邻格泄漏
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;   // 1 级占位
    samplerInfo.maxLod = 1.0f;
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
