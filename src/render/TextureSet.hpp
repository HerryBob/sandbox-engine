#pragma once
#include <cstdint>

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

namespace engine::render {

// ============================================================================
// TextureSet：一组"只读采样纹理"资源（图 → VkImage → VkImageView → VkSampler）
// ============================================================================
//
// 从 VulkanContext 抽出的第二块职责（S3 第一刀）。它只管**GPU 侧**的图与采样器：
//   · 上传：调用方给 CPU 像素（RGBA8），本类负责 createImage → staging → 布局转换
//   · 对外只暴露 view()/sampler()，供描述符写入使用
//   · CPU 侧的图集构建（哪些纹理、拼到哪一格、UV 是多少）**不在这里** ——
//     那属于资源/网格构建侧（TextureAtlas），本类不该知道"方块纹理长什么样"。
//
// 为什么这么切：S5 拆 ChunkMeshBuilder 时，网格构建需要 TextureAtlas（CPU），
// 而绘制需要 view/sampler（GPU）。把这两者分开，网格构建就不必依赖 Vulkan 句柄。
//
// ---- 未来接缝（不实现，只留位置）----
//   · 光影：多张纹理（法线/粗糙度/阴影图）会以 TextureSet 数组的形式出现；
//     本类已按"一组图 + 一个采样器"建模，扩成多组即可。
//   · 纹理数组 / bindless：view() 会改成 view(index)，采样器仍只有一个。
class TextureSet {
public:
    TextureSet() = default;
    ~TextureSet() = default;

    // 上传一张 RGBA8 图并建好 view + sampler。失败返回 false（错误已打日志）。
    bool init(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue graphicsQueue,
              VkCommandPool cmdPool, const void* rgba, std::uint32_t width,
              std::uint32_t height);

    // 逆序销毁（重复调用安全）。
    void shutdown();

    VkImageView view() const { return view_; }
    VkSampler  sampler() const { return sampler_; }
    bool       ready() const { return view_ != VK_NULL_HANDLE && sampler_ != VK_NULL_HANDLE; }

private:
    bool createImage(const void* rgba, std::uint32_t width, std::uint32_t height);
    bool createImageView();
    bool createSampler();

    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice         device_ = VK_NULL_HANDLE;
    VkQueue          graphicsQueue_ = VK_NULL_HANDLE;
    VkCommandPool    cmdPool_ = VK_NULL_HANDLE;

    VkImage        image_ = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory_ = VK_NULL_HANDLE;
    VkImageView    view_ = VK_NULL_HANDLE;
    VkSampler      sampler_ = VK_NULL_HANDLE;
};

} // namespace engine::render
