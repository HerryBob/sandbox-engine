#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

#include "shared/state/CameraState.hpp"

#include "render/hud/HudInfo.hpp"
#include "render/hud/HudAtlas.hpp"
#include "render/hud/HudTextBuilder.hpp"

namespace engine::render {

// ============================================================================
// HudRenderer：游戏内调试浮层（F3）—— 字体纹理 + 独立管线 + 每帧顶点缓冲
// ============================================================================
//
// 从 VulkanContext 抽出的第一块"渲染职责"（S2）。它是整个渲染层里最独立的一块：
//   · 自己的描述符池/布局/管线（不与 3D 抢额度）
//   · 自己的字体纹理与每帧持久映射顶点缓冲
//   · 画在同一个 renderPass 的最后一趟（Cutout 之后、EndRenderPass 之前）
//   · 只依赖 6 个外部句柄（设备/物理设备/队列/命令池/renderPass/交换链尺寸）
//
// 与 VulkanContext 的边界：
//   VulkanContext 只保留 setHudVisible / setHudStats / hudVisible 三个薄转发，
//   资源创建 init()、交换链变更 setExtent()、绘制 draw() 都由本类负责。
//
// ---- 未来接缝（不实现，只留位置）----
//   · 光影：HUD 必须在后处理之后（否则被色调映射/曝光影响）。届时 draw() 会
//     接收"在哪个 pass 之后"的参数，或由 FrameRecorder 的 pass 列表决定调用点。
//   · HUD 改成 2D 图元（图标/血条）：本类已有顶点流 + 字体图集，扩成图集多格即可。
//   · HUD 将来要**以 DLL 模组形式提供**：所以本类刻意只依赖 6 个 Vulkan 句柄 + POD 统计
//     （不引用 VulkanContext、不碰业务/物理对象）。模组化时把这 6 个句柄经一个精简的
//     "渲染注入接口"传进去即可，本类内部实现不用改。
class HudRenderer {
public:
    static constexpr std::size_t kHudVertexCapacity = 12288;   // 2048 字符 × 6 顶点
    static constexpr uint32_t    kMaxFramesInFlight = 2;

    HudRenderer() = default;

    // init 时调用一次：创建字体纹理 / 描述符 / 管线 / 每帧顶点缓冲。
    bool init(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue graphicsQueue,
              VkCommandPool cmdPool, VkRenderPass renderPass, VkExtent2D extent,
              VkSampleCountFlagBits msaaSamples);
    // 逆序释放（重复调用安全）。设备句柄在 init 时存下，这里不需要参数。
    void shutdown();

    // 字体图集句柄（供其它 2D 元素借用，例如准星）：只有 HUD 创建/持有它。
    // 返回 POD 而非引用：调用方拿不到本类内部状态，也不会误释放（模组化后的接口形态）。
    hud::HudAtlas atlas() const {
        return hud::HudAtlas{ fontImage_, fontImageView_, fontSampler_ };
    }

    // 相机槽（HUD 显示坐标/朝向用）：attachCameraState 时注入，不持有所有权
    void setCameraState(const engine::shared::CameraState* cam) { cameraState_ = cam; }

    // 交换链重建后更新尺寸（HUD 的 push constant 用屏幕尺寸换算）。
    void setExtent(VkExtent2D extent) { swapchainExtent_ = extent; }

    // 可见性（F3 切换）：打印日志便于区分"没送到"与"画错"
    void setHudVisible(bool visible);
    bool visible() const { return hudVisible_; }
    // 业务统计（由渲染线程按 1Hz 喂）
    void setHudStats(long long chunkCount, long long blockCount,
                     long long tickCount, double simTime);
    // 帧统计（FPS / 帧耗时 / 已呈现帧数）
    void setHudFrameStats(double fps, double frameMs, std::uint64_t frames) {
        hudFps_ = fps; hudFrameMs_ = frameMs; hudFrames_ = frames;
    }
    // 网格统计（已提交的 chunk 网格数 + 三角形数）：由 VulkanContext 每帧喂，
    // HUD 不再直接遍历 chunkMeshes_（那属于网格存储的职责，S5 会再拆）。
    void setMeshStats(int meshChunks, long long triangles) {
        meshChunks_ = meshChunks;
        meshTriangles_ = triangles;
    }

    // 在 EndRenderPass 之前录制 HUD 这一趟。
    void drawHud(VkCommandBuffer cmd, uint32_t frameIndex);

    // 覆盖层：非空时**替代**调试统计行（用于存档选择这类模态界面）。
    // 只用 ASCII（字体图集只覆盖 32..126），不做布局/交互 —— 交互由调用方处理。
    void setOverlayLines(std::vector<std::string> lines) { overlayLines_ = std::move(lines); }
    void clearOverlayLines() { overlayLines_.clear(); }

private:
    bool        createHudResources();      // 一次性创建：字体纹理 / 描述符 / 管线 / 顶点缓冲
    void        destroyHudResources();     // 逆序释放（重复调用安全）
    bool        createFontTexture();
    bool        createHudPipeline();
    std::size_t buildHudVertices();

    // ---- 外部句柄（init 时填入）----
    VkPhysicalDevice       physicalDevice_ = VK_NULL_HANDLE;
    VkDevice               device_ = VK_NULL_HANDLE;
    VkQueue                graphicsQueue_ = VK_NULL_HANDLE;
    VkCommandPool          cmdPool_ = VK_NULL_HANDLE;
    VkRenderPass           renderPass_ = VK_NULL_HANDLE;
    VkExtent2D             swapchainExtent_{};
    VkSampleCountFlagBits  msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    const engine::shared::CameraState* cameraState_ = nullptr;   // 只读引用（HUD 显示用）
    int       meshChunks_ = 0;                                   // 已提交网格的 chunk 数
    long long meshTriangles_ = 0;                                // 三角形总数

    // ---- 自有资源 ----
    hud::HudInfo         hudInfo_{};            // 业务统计快照
    bool                 hudVisible_ = false;   // F3 切换，默认隐藏
    std::vector<std::string> overlayLines_;    // 非空 = 模态覆盖层（存档菜单等）
    bool                 hudDrawnOnce_ = false; // 首帧绘制日志只打一次
    hud::HudTextBuilder  hudText_;              // 逐帧重建的顶点流（CPU 侧）
    // 帧统计（由 VulkanContext 每帧喂入：它在那里算，HUD 只负责显示）
    double            hudFps_ = 0.0;
    double            hudFrameMs_ = 0.0;
    std::uint64_t     hudFrames_ = 0;
    std::chrono::steady_clock::time_point tpsWindowStart_{};
    long long         tpsLastTicks_ = 0;

    VkImage              fontImage_ = VK_NULL_HANDLE;
    VkDeviceMemory       fontImageMemory_ = VK_NULL_HANDLE;
    VkImageView          fontImageView_ = VK_NULL_HANDLE;
    VkSampler            fontSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout hudDescLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      hudDescPool_ = VK_NULL_HANDLE;   // 独立池，不与 3D 抢额度
    VkDescriptorSet       hudDescSet_ = VK_NULL_HANDLE;
    VkPipelineLayout      hudPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            hudPipeline_ = VK_NULL_HANDLE;
    VkBuffer          hudVertexBuffer_[kMaxFramesInFlight]{};
    VkDeviceMemory    hudVertexMemory_[kMaxFramesInFlight]{};
    void*             hudVertexMapped_[kMaxFramesInFlight]{};   // 持久映射（HOST_COHERENT）
};

} // namespace engine::render
