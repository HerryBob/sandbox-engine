#pragma once
#include <cstdint>

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

namespace engine::render {

// ============================================================================
// PipelineSet：渲染通道 + MSAA/深度附件 + 主图形管线
// ============================================================================
//
// 从 VulkanContext 抽出的第三块职责（S4a）。这三样是**一起**出现的：
//   · renderPass 描述了附件（颜色 MSAA + 解析目标 + 深度）
//   · 深度/颜色**附件资源**必须与 renderPass 的格式/采样数一致
//   · 管线又必须引用 renderPass 与描述符布局
// 所以它们只能一起搬，单独搬任何一个都会留下"格式与附件不一致"的隐患。
//
// 依赖（init 时注入，之后不向外部索取）：
//   device / physicalDevice / swapchainFormat / swapchainExtent
//   msaaSamples / depthFormat（由 DeviceContext 选定的设备属性）
//   descLayout（管线布局要绑定它；仍由 VulkanContext 持有，S4b 归入 FrameInfra）
// 产出：
//   renderPass() / depthView() / colorView() —— 帧缓冲创建与 drawFrame 要用
//   pipelineLayout() / pipeline()
//
// ---- 未来接缝（不实现，只留位置）----
//   · 光影：需要**多个** renderPass（shadow → main → post）与多套管线。
//     本类按"一个 Set = 一组附件+通道+管线"建模，届时加 ShadowPipelineSet /
//     PostPipelineSet，由 FrameRecorder 的 pass 列表决定顺序与依赖。
//   · 世界 LOD：不同 LOD 若需要不同着色器（简化几何），这里加一条 pipeline，
//     按 lod 选择；现在只有一条。
class PipelineSet {
public:
    PipelineSet() = default;

    // ① 附件 + 渲染通道：只要设备格式与尺寸（不依赖描述符布局）
    bool initAttachmentsAndPass(VkDevice device, VkPhysicalDevice physicalDevice,
                                VkFormat swapchainFormat, VkExtent2D extent,
                                VkSampleCountFlagBits msaaSamples, VkFormat depthFormat);
    // ② 主图形管线：**必须**在 createUniformObjects 之后（要 descLayout）
    bool initPipeline(VkDescriptorSetLayout descLayout);

    // 交换链重建后更新尺寸（附件要按新尺寸重建，故由调用方在 recreate 后重新 init）
    bool recreateAttachments(VkExtent2D extent);

    // 逆序销毁（重复调用安全）：管线 → 布局 → 附件 → 通道
    void shutdown();

    VkRenderPass     renderPass() const { return renderPass_; }
    VkPipelineLayout pipelineLayout() const { return pipelineLayout_; }
    VkPipeline       pipeline() const { return pipeline_; }
    VkPipeline       cutoutPipeline() const { return cutoutPipeline_; }   // Cutout 桶（双面）
    VkImageView      depthView() const { return depthImageView_; }
    VkImageView      colorView() const { return colorBufferView_; }
    VkExtent2D       extent() const { return swapchainExtent_; }

private:
    bool createRenderPass();
    bool createDepthResources();
    bool createColorResources();
    bool createPipeline();

    // ---- 外部注入 ----
    VkDevice              device_ = VK_NULL_HANDLE;
    VkPhysicalDevice      physicalDevice_ = VK_NULL_HANDLE;
    VkFormat              swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D            swapchainExtent_{};   // 与导出代码同名，避免逐处改名
    VkSampleCountFlagBits msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    VkFormat              depthFormat_ = VK_FORMAT_UNDEFINED;
    VkDescriptorSetLayout descLayout_ = VK_NULL_HANDLE;

    // ---- 本类持有 ----
    VkRenderPass     renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline       pipeline_ = VK_NULL_HANDLE;
    VkPipeline       cutoutPipeline_ = VK_NULL_HANDLE;
    VkImage          depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory   depthImageMemory_ = VK_NULL_HANDLE;
    VkImageView      depthImageView_ = VK_NULL_HANDLE;
    VkImage          colorBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory   colorBufferMemory_ = VK_NULL_HANDLE;
    VkImageView      colorBufferView_ = VK_NULL_HANDLE;
};

} // namespace engine::render
