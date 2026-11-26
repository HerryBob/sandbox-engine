#pragma once
#include <cstdint>
#include <vector>

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

namespace engine::render {

// ============================================================================
// FrameInfra：每帧复用的"帧基础设施"
// ============================================================================
//
// 从 VulkanContext 抽出的第四块职责（S4b）。包含：
//   · 命令池 + 每帧一条命令缓冲（kMaxFramesInFlight 轮转）
//   · 同步对象：imageAvailable 信号量（每帧槽）+ renderFinished（每交换链图像）+ inFlight 栅栏
//   · UBO（历史遗留：相机矩阵已改走 push constant）+ 描述符布局/池/集
//
// 为什么这些放一起：它们都由"帧槽数"驱动，且在 drawFrame 里成组使用
// （等 fence → 重置命令缓冲 → 绑定描述符集 → submit 时用信号量/栅栏）。
// 拆开会让每一方都要知道帧槽编号，反而更乱。
//
// 依赖（init 注入）：device / physicalDevice / graphicsQueue / swapchainImageCount
// 对外：cmdBuffer(i) / imageAvailable(i) / renderFinished(i) / inFlightFence(i)
//       descSet() / descLayout() / commandPool()
//
// ---- 未来接缝（不实现，只留位置）----
//   · 光影：多 pass 可能需要**多条命令缓冲**（一次 submit 多个 pass）或次级命令缓冲，
//     届时把 cmdBuffer(i) 扩成 cmdBuffer(pass, i)。
//   · 帧槽数改成 3（更平滑的 CPU/GPU 并行）：只需把 kMaxFramesInFlight 变成构造参数。
class FrameInfra {
public:
    static constexpr uint32_t kMaxFramesInFlight = 2;

    FrameInfra() = default;

    // ① 命令池 + 命令缓冲 + 同步对象：先于一切需要命令缓冲的上传（纹理/网格）
    bool initCommandAndSync(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue graphicsQueue,
                            uint32_t graphicsQueueFamily, uint32_t swapchainImageCount);
    // ② UBO + 描述符布局/池/集：要在方块图集创建之后（binding 1 绑图集）
    bool initDescriptors(VkImageView atlasView, VkSampler atlasSampler);
    void shutdown();

    VkCommandPool   commandPool() const { return cmdPool_; }
    VkCommandBuffer cmdBuffer(uint32_t frame) const { return cmdBuffers_[frame]; }
    VkSemaphore     imageAvailable(uint32_t frame) const { return imageAvailable_[frame]; }
    VkSemaphore     renderFinished(uint32_t imageIndex) const { return renderFinished_[imageIndex]; }
    VkFence         inFlightFence(uint32_t frame) const { return inFlightFences_[frame]; }

    VkDescriptorSet       descSet() const { return descSet_; }
    VkDescriptorPool      descPool() const { return descPool_; }
    VkBuffer              uboBuffer() const { return uboBuffer_; }
    VkDescriptorSetLayout descLayout() const { return descLayout_; }

private:
    bool createCommandObjects();
    bool createUniformObjects();
    bool createSyncObjects();
    void destroyUniformObjects();

    VkDevice       device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkQueue        graphicsQueue_ = VK_NULL_HANDLE;
    uint32_t       graphicsQueueFamily_ = 0;
    VkImageView    atlasView_ = VK_NULL_HANDLE;
    VkSampler      atlasSampler_ = VK_NULL_HANDLE;
    uint32_t       swapchainImageCount_ = 0;

    VkCommandPool   cmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmdBuffers_[kMaxFramesInFlight]{};
    VkSemaphore     imageAvailable_[kMaxFramesInFlight]{};
    std::vector<VkSemaphore> renderFinished_;
    VkFence         inFlightFences_[kMaxFramesInFlight]{};

    // UBO：已废弃（相机矩阵走 push constant），layout/set 仅为兼容旧 binding 0
    VkBuffer       uboBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory uboMemory_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet       descSet_ = VK_NULL_HANDLE;
};

} // namespace engine::render
