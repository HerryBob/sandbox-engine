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
#include "shared/Block.hpp"

// 依赖方案 A：LunarG SDK 直连（find_package(Vulkan) + vulkan-1.lib 静态链接）。
// 不再用 volk 动态加载（旧引擎 volk 方案在迁移时切换为 SDK 直连）。
// Windows 平台 surface 扩展（vkCreateWin32SurfaceKHR）需要平台宏才会被引入。
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
//用户输入
#include "shared/PlayerInput.hpp"
namespace engine::render {

// 体素顶点：位置 + 颜色（纹理阶段再扩展 UV）。
// 布局必须与管线顶点输入描述（binding/attribute）一致。
struct Vertex {
    glm::vec3 pos;   // 方块局部坐标（-0.5..0.5 单位立方体）
    glm::vec3 color; // 顶点颜色（本阶段逐面着色，代替纹理）
    glm::vec2 uv; //  顶点纹理坐标（本阶段逐面着色，代替纹理）
};

// 顶点着色器 uniform：三个矩阵分开传（MVP 拆解）。
// model=方块世界变换（目前单位矩阵），view=相机视角，proj=投影。
// 着色器侧 gl_Position = proj * view * model * pos。
struct UniformBufferObject {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
};

class VulkanContext {
public:
    static constexpr const char* kCameraHelp = "相机控制：WASD=平移 空格/左Shift=上下 鼠标左键拖动=旋转 R=重置视角\n";
    void attachPlayerInput(engine::shared::PlayerInput* input) { playerInput_ = input; }
public:
    VulkanContext() = default;
    ~VulkanContext();
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

	//以下四个函数声明就是窗口的四个基本操作，分别是初始化窗口、轮询事件、检查是否关闭窗口以及关闭窗口。
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
    bool rebuildMeshFromBlocks(const std::vector<engine::shared::Block>& blocks); // 重建顶点/索引缓冲：从主世界读取方块数据


private:
    bool createInstance();
    bool createSurface();
    bool pickDevice();
    bool createDevice();
    bool createSwapchain();
    void destroySwapchain();
    bool recreateSwapchain();        // 窗口尺寸变化：销毁重建尺寸相关资源
    void destroyColorResources();    // MSAA 颜色图三件套（recreate 与 shutdown 共用）
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
    bool createColorResources();
    bool createTextureImage();      // staging 搬运：CPU 写临时缓冲 → GPU 拷进显存
    bool createTextureImageView();  // 视图：图像 → 可采样视图
    bool createTextureSampler();    // 采样器：告诉 GPU 怎么取样（过滤/寻址）
    void updateCamera(double dt); // 读输入更新相机状态 + 写 UBO（drawFrame 开头调用）
    void destroyUniformObjects();
    

    // GLFW 回调（static 成员：可作 C 函数指针传参，同时能访问私有成员）。
    static void mouseButtonCb(GLFWwindow* w, int button, int action, int mods);
    static void cursorPosCb(GLFWwindow* w, double x, double y);
    static void framebufferResizeCallback(GLFWwindow* w, int width, int height);

    
    //用户输入
    engine::shared::PlayerInput* playerInput_ = nullptr; // 玩家输入共享槽（外部挂载，非拥有）
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

    // 深度缓冲
    VkImage              depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory       depthImageMemory_ = VK_NULL_HANDLE;
    VkImageView          depthImageView_ = VK_NULL_HANDLE;
    VkFormat             depthFormat_ = VK_FORMAT_UNDEFINED;

    // MSAA 抗锯齿
    VkSampleCountFlagBits msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    VkImage         colorBuffer_ = VK_NULL_HANDLE;       // MSAA 颜色图（绘制目标）
    VkDeviceMemory  colorBufferMemory_ = VK_NULL_HANDLE;
    VkImageView     colorBufferView_ = VK_NULL_HANDLE;

    // 渲染对象（第三步：RenderPass + Pipeline + 帧缓冲 + 命令 + 同步）
    static constexpr uint32_t kMaxFramesInFlight = 2;
    VkRenderPass         renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout     pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline           pipeline_ = VK_NULL_HANDLE;
    bool                 framebufferResized_ = false; // 窗口尺寸变化标志（GLFW 回调置位）
    std::vector<VkFramebuffer> framebuffers_;
    VkBuffer             vertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory       vertexBufferMemory_ = VK_NULL_HANDLE;
    VkBuffer             indexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory       indexBufferMemory_ = VK_NULL_HANDLE;
    uint32_t             vertexCount_ = 0;
    uint32_t             indexCount_ = 0;
    VkCommandPool        cmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer      cmdBuffers_[kMaxFramesInFlight]{}; // 每帧槽一条（2 组轮转）
    VkSemaphore          imageAvailable_[kMaxFramesInFlight]{}; // acquire 信号量：2 组轮转
    std::vector<VkSemaphore> renderFinished_;
    VkFence              inFlightFences_[kMaxFramesInFlight]{};
    bool                 renderObjectsReady_ = false;
    std::uint64_t        framesPresented_ = 0;

    // ---- 相机（MVP uniform）----
    glm::vec3  camPos_{0.0f, 0.0f, 3.5f};
    float      camYaw_ = 0.0f;    // 左右旋转（弧度）——仅渲染线程写
    float      camPitch_ = 0.0f;  // 上下旋转（弧度，clamp ±85°）——仅渲染线程写

    // ---- 输入槽

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

    //加入纹理链
    VkImage         textureImage_ = VK_NULL_HANDLE;
    VkDeviceMemory  textureImageMemory_ = VK_NULL_HANDLE;
    VkImageView     textureImageView_ = VK_NULL_HANDLE;
    VkSampler       textureSampler_ = VK_NULL_HANDLE;
    uint32_t        texWidth_ = 0;
    uint32_t        texHeight_ = 0;
};

} // namespace engine::render
