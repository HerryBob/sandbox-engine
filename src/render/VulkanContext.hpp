#pragma once
// 渲染上下文：GLFW 窗口 + Vulkan 实例 + 物理设备枚举 + 逻辑设备 + surface + 交换链
// + RenderPass + 图形管线 + 帧缓冲 + 每帧绘制循环（acquire→record→submit→present）。
// 2026-09-30：第一步窗口/实例/设备；第二步 surface/交换链；
//            第三步 RenderPass/Pipeline + 第一个三角形（第一帧上屏里程碑）。
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>

#include <glm/glm.hpp>

// volk 硬性要求：任何 vulkan.h 被 include 前必须先定义 VK_NO_PROTOTYPES
// （禁用 vulkan 函数原型声明，全部函数指针由 volk 运行时加载提供）。
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
// Windows 平台 surface 扩展（vkCreateWin32SurfaceKHR）需要平台宏才会被引入。
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <volk.h>

namespace engine::render {

// 体素顶点：位置 + 颜色（纹理阶段再扩展 UV）。
// 布局必须与管线顶点输入描述（binding/attribute）一致。
struct Vertex {
    glm::vec3 pos;   // 方块局部坐标（-0.5..0.5 单位立方体）
    glm::vec3 color; // 顶点颜色（本阶段逐面着色，代替纹理）
};

// 顶点着色器 uniform：MVP 矩阵（每帧更新）。
// 本阶段模型=单位矩阵（方块在原点），只传 proj*view；相机里程碑后再拆开。
struct UniformBufferObject {
    glm::mat4 mvp;
};

class VulkanContext {
public:
    static constexpr const char* kCameraHelp =
        "相机控制：WASD=平移 空格/左Shift=上下 鼠标左键拖动=旋转 R=重置视角\n";
public:
    VulkanContext() = default;
    ~VulkanContext();
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // 初始化链：glfwInit → 窗口 → volk 加载 → 实例 → surface →
    // 枚举设备（graphics+present 队列族）→ 逻辑设备 → 交换链 + 图像视图。
    // 失败返回 false 并打印原因。
    bool init(int width, int height, const char* title);

    // 主线程轮询窗口事件（渲染线程不触碰窗口对象）。
    void pollEvents();

    // 用户是否点了窗口关闭按钮。
    bool shouldClose() const;

    void shutdown();

    // ---- 查询（断言/日志用）----
    GLFWwindow*       window() const { return window_; }
    VkInstance        instance() const { return instance_; }
    VkSurfaceKHR      surface() const { return surface_; }
    VkPhysicalDevice  physicalDevice() const { return physicalDevice_; }
    VkDevice          device() const { return device_; }
    VkQueue           graphicsQueue() const { return graphicsQueue_; }
    uint32_t          graphicsQueueFamily() const { return graphicsQueueFamily_; }
    VkSwapchainKHR    swapchain() const { return swapchain_; }
    VkFormat          swapchainFormat() const { return swapchainFormat_; }
    VkExtent2D        swapchainExtent() const { return swapchainExtent_; }
    std::size_t       swapchainImageCount() const { return swapchainImages_.size(); }
    VkImageView       swapchainImageView(std::size_t i) const {
        return i < swapchainImageViews_.size() ? swapchainImageViews_[i] : VK_NULL_HANDLE;
    }
    const std::vector<VkPhysicalDevice>& devices() const { return devices_; }
    // 每帧绘制（渲染线程调用）：acquire → 录制 → submit → present。
    // 返回 false = 提交/呈现失败（交换链过期除外，返回 true 跳过本帧）。
    bool drawFrame();
    // 成功呈现过的帧数（断言用）。
    std::uint64_t framesPresented() const { return framesPresented_; }
    VkRenderPass  renderPass() const { return renderPass_; }
    VkPipeline    pipeline() const { return pipeline_; }
    VkDescriptorSet descriptorSet() const { return descSet_; }
    uint32_t      vertexCount() const { return vertexCount_; }
    uint32_t      indexCount() const { return indexCount_; }

private:
    bool createInstance();
    bool createSurface();
    bool pickDevice();
    bool createDevice();
    bool createSwapchain();
    void destroySwapchain();
    bool createRenderPass();
    bool createUniformObjects();
    bool createDepthResources();
    bool createPipeline();
    bool createFramebuffers();
    bool createMeshBuffers();
    bool createCommandObjects();
    bool createSyncObjects();
    void destroyRenderObjects();
    void destroyDepthResources();
    void updateCamera(double dt); // 读输入更新相机状态 + 写 UBO（drawFrame 开头调用）
    void destroyUniformObjects();

    // GLFW 回调（static 成员：可作 C 函数指针传参，同时能访问私有成员）。
    static void mouseButtonCb(GLFWwindow* w, int button, int action, int mods);
    static void cursorPosCb(GLFWwindow* w, double x, double y);

    GLFWwindow* window_ = nullptr;
    bool        glfwInitialized_ = false;
    VkInstance  instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice    device_ = VK_NULL_HANDLE;
    VkQueue     graphicsQueue_ = VK_NULL_HANDLE;
    uint32_t    graphicsQueueFamily_ = UINT32_MAX;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat    swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D  swapchainExtent_{0, 0};
    std::vector<VkImage>      swapchainImages_;
    std::vector<VkImageView>  swapchainImageViews_;
    std::vector<VkPhysicalDevice> devices_;

    // 深度缓冲（2026-10-01：渲染正确性地基——深度测试/背面剔除前的必要条件）
    VkImage              depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory       depthImageMemory_ = VK_NULL_HANDLE;
    VkImageView          depthImageView_ = VK_NULL_HANDLE;
    VkFormat             depthFormat_ = VK_FORMAT_UNDEFINED;

    // 渲染对象（第三步：RenderPass + Pipeline + 帧缓冲 + 命令 + 同步）
    VkRenderPass         renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout     pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline           pipeline_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
    VkBuffer             vertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory       vertexBufferMemory_ = VK_NULL_HANDLE;
    VkBuffer             indexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory       indexBufferMemory_ = VK_NULL_HANDLE;
    uint32_t             vertexCount_ = 0;
    uint32_t             indexCount_ = 0;
    VkCommandPool        cmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer      cmdBuffer_ = VK_NULL_HANDLE;
    VkSemaphore          imageAvailable_ = VK_NULL_HANDLE;
    VkSemaphore          renderFinished_ = VK_NULL_HANDLE;
    VkFence              inFlightFence_ = VK_NULL_HANDLE;
    bool                 renderObjectsReady_ = false;
    std::uint64_t        framesPresented_ = 0;

    // ---- 相机（MVP uniform）----
    glm::vec3  camPos_{0.0f, 0.0f, 3.5f};
    float      camYaw_ = 0.0f;    // 左右旋转（弧度）——仅渲染线程写
    float      camPitch_ = 0.0f;  // 上下旋转（弧度，clamp ±85°）——仅渲染线程写

    // ---- 输入槽（2026-10-01：消除相机成员数据竞争）----
    // 主线程 GLFW 回调只写这些原子槽；渲染线程 updateCamera 消费增量。
    // camYaw_/camPitch_/camPos_ 等相机成员从此只被渲染线程写 → 零竞争。
    // （修复前：回调直接写相机成员 ↔ 渲染线程 updateCamera 并发读写 = 数据竞争
    //   UB，未定义写随栈布局漂移，表现为 createFramebuffers/uboMemory_ 等随机写坏。）
    std::atomic<bool>   inputDragging_{false}; // 左键是否按下（回调写）
    std::atomic<double> inputLastX_{0.0};      // 上次光标位置（回调写）
    std::atomic<double> inputLastY_{0.0};
    std::atomic<double> inputYawDelta_{0.0};   // 待消费旋转增量（回调累加 / 渲染 exchange 取走）
    std::atomic<double> inputPitchDelta_{0.0};
    VkBuffer             uboBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory       uboMemory_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool     descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet      descSet_ = VK_NULL_HANDLE;
};

} // namespace engine::render
