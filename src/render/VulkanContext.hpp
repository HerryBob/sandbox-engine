#pragma once
// 渲染上下文：GLFW 窗口 + Vulkan 实例 + 物理设备枚举 + 逻辑设备 + surface + 交换链
// + RenderPass + 图形管线 + 帧缓冲 + 每帧绘制循环（acquire→record→submit→present）。
// 2026-09-30：第一步窗口/实例/设备；第二步 surface/交换链；
//            第三步 RenderPass/Pipeline + 第一个三角形（第一帧上屏里程碑）。
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "shared/chunk/ChunkPos.hpp"
#include "render/assets/TextureAtlas.hpp"
#include "render/BodyMeshStore.hpp"
#include "render/ChunkMeshStore.hpp"
#include "render/EntityRenderer.hpp"
#include "render/MeshTypes.hpp"
#include "render/FrameInfra.hpp"
#include "render/HudRenderer.hpp"
#include "render/hud/CrosshairRenderer.hpp"
#include "render/PipelineSet.hpp"
#include "render/TextureSet.hpp"
#include "render/hud/HudInfo.hpp"
#include "render/hud/HudTextBuilder.hpp"

// 依赖方案 A：LunarG SDK 直连（find_package(Vulkan) + vulkan-1.lib 静态链接）。
// 不再用 volk 动态加载（旧引擎 volk 方案在迁移时切换为 SDK 直连）。
// Windows 平台 surface 扩展（vkCreateWin32SurfaceKHR）需要平台宏才会被引入。
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
//用户输入
#include "shared/state/CameraState.hpp"
namespace engine::render {

// 体素顶点：位置 + 颜色（纹理阶段再扩展 UV）。
// 布局必须与管线顶点输入描述（binding/attribute）一致。
struct UniformBufferObject {
    glm::mat4 viewProj;   // = proj * view  （model 恒为单位阵，已折进来）
};

// 一个 chunk 的网格（GPU 资源 + 计数）。key = ChunkPos（ChunkPos.hpp 已提供 hash）。
// 增量重建：每个 chunk 独立持有自己的顶点/索引缓冲，脏 chunk 只重建自己那份；
// 全量接口保留为"重建全部 chunk"（首帧/世界重建用）。
// 分桶：一个 chunk 可同时持有 Solid 桶 + Cutout 桶两组网格（玻璃等），
//       重建/销毁同一个对象，生命周期原子化（不拆两个 map）。

class VulkanContext {
public:
    void attachCameraState(engine::shared::CameraState* camera) {
        cameraState_ = camera;
        hud_.setCameraState(camera);   // HUD 显示相机坐标/朝向
    }

    // ---- HUD（游戏内调试浮层，F3 开关）----
    // 业务统计由渲染线程按 1Hz 调用：chunkCount/blockCount 要抢业务锁，别每帧查。
    // 实现已抽到 HudRenderer（S2），这里只做转发。
    void setHudStats(long long chunkCount, long long blockCount,
                     long long tickCount, double simTime) {
        hud_.setHudStats(chunkCount, blockCount, tickCount, simTime);
    }
    // 可见性由渲染线程按 F3 按下边沿切换（默认隐藏，不打扰正常游玩）。
    void setHudVisible(bool visible) { hud_.setHudVisible(visible); }
    bool hudVisible() const { return hud_.visible(); }
public:
    VulkanContext() = default;
    ~VulkanContext();
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

	//以下四个函数声明就是窗口的四个基本操作，分别是初始化窗口、轮询事件、检查是否关闭窗口以及关闭窗口。
    bool init(int width, int height, const char* title);

    // 主线程轮询窗口事件（渲染线程不触碰窗口对象）。
    void pollEvents();
    // 键盘查询（菜单用）：直接问 GLFW 当前键状态。菜单只在装配期跑（单线程），
    // 不与 InputSampler 的按键回调冲突。
    bool keyDown(int glfwKey) const;
    // 模态覆盖层转发（存档菜单用）：非空时 HUD 只画这些行，不画调试统计。
    void setHudOverlay(std::vector<std::string> lines) { hud_.setOverlayLines(std::move(lines)); }
    void clearHudOverlay() { hud_.clearOverlayLines(); }

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
    VkRenderPass  renderPass() const { return pipelines_.renderPass(); }
    VkPipeline    pipeline() const { return pipelines_.pipeline(); }
    VkPipeline    cutoutPipeline() const { return pipelines_.cutoutPipeline(); }
    VkDescriptorSet descriptorSet() const { return infra_.descSet(); }
    bool rebuildMeshFromBlocks(const std::vector<engine::shared::Block>& blocks); // 修改为增量重建chunk
    bool rebuildChunkMeshes(const std::vector<engine::shared::Block>& blocks, const std::unordered_set<engine::shared::ChunkPos>& dirtyChunks); // 增量重建：只重建 dirtyChunks 里的 chunk

    // ---- 刚体网格（可动方块组）----
    // 结构变化时上传/替换（打包、方块变动）；位姿每帧只改 bodyPos_/bodyRot_，不碰缓冲。
    // blocks：刚体局部坐标方块列表（网格由图集 + 方块注册表现场生成）。
    // 只能在渲染线程（或初始化期尚未启线程时）调用：VkBuffer 创建非线程安全。
    bool uploadBodyMesh(int contraptionId, const std::vector<engine::shared::Block>& blocks);
    // 位姿 = 位置 + 旋转（四元数，glm 顺序 w,x,y,z 由调用方按 {qw,qx,qy,qz} 传入）。
    // 刚体顶点是局部坐标，model matrix = translate(pos) × mat4_cast(rot)。
    // 直接设置已插值好的位姿（PoseHistory 路径）：本类不再做任何插值。
    void setBodyPoseDirect(int contraptionId, double x, double y, double z,
                           float qx, float qy, float qz, float qw);
    void destroyBodyMesh(int contraptionId);

    // ---- 实体模型（带骨骼，逐骨骼 draw）----
    // 加载 bbmodel + 独立贴图，按骨骼切分网格上传。id 与刚体 id 是两套编号。
    // 只能在渲染线程（或初始化期尚未启线程时）调用：VkBuffer 创建非线程安全。
    bool loadEntityModel(int entityId, const std::string& bbmodelPath,
                         const std::string& texturePath);
    void setEntityPose(int entityId, double x, double y, double z, float yawDeg);
    // 实体可见性：false 时本趟跳过该实体（第一人称要把自身模型藏起来，
    // 否则相机正好位于头/躯干内部，视野被自己的模型糊住）。
    void setEntityVisible(int entityId, bool visible);
    void destroyEntityModel(int entityId);


private:
    bool createInstance();
    bool createSurface();
    bool pickDevice();
    bool createDevice();
    bool createSwapchain();
    void destroySwapchain();
    bool recreateSwapchain();        // 窗口尺寸变化：销毁重建尺寸相关资源
    bool createFramebuffers();
    void destroyRenderObjects();
    // 确保某张实体贴图已加载（图像/视图/采样器 + 自己的 descriptor set）。
    // 按路径去重：重复调用同一路径只建一次。返回 false 表示该贴图不可用。
    void updateViewProj(double dt); // 从 CameraState 读相机状态 → 算 viewProj（drawFrame 开头调用，只读不处理输入）
    // 推送「模型矩阵 + 相机矩阵」共 128 字节到 push constant（两个 mat4，正好是规范保证的下限）。
    // 相机矩阵以前放在单份 UBO 里，而 kMaxFramesInFlight=2 → CPU 写第 N 帧会覆盖第 N-1 帧
    // 还在读的内容，偶发一帧的相机矩阵错乱（表现为一片地形变天空色）。改走 push constant 后
    // 每个命令缓冲自带一份，不再有跨帧共享。
    void pushDrawMatrices(VkCommandBuffer cmd, const glm::mat4& bodyModel) const;

    // ---- 每帧录制的拆分（S7a：drawFrame 268 行 → 帧首/录制/提交 + 5 个 pass）----
    void updateFrameStats();                                   // FPS/帧耗时窗口
    bool beginFrame(uint32_t frame, uint32_t& imageIndex);      // 等 fence → 回收 → acquire
    bool recordFrame(uint32_t frame, uint32_t imageIndex);      // begin cmd → renderPass → 五趟 → end
    void recordChunkSolidPass(VkCommandBuffer cmd);             // ① Solid（不透明）
    void recordBodyPass(VkCommandBuffer cmd);                   // ② 刚体
    void recordEntityPass(VkCommandBuffer cmd);                 // ③ 实体（骨骼）
    void recordChunkCutoutPass(VkCommandBuffer cmd);            // ④ Cutout（镂空/双面）
    void recordHudPass(VkCommandBuffer cmd, uint32_t frame);    // ⑤ HUD
    bool submitFrame(uint32_t frame, uint32_t imageIndex);      // submit + present

    

    // GLFW 回调（static 成员：可作 C 函数指针传参，同时能访问私有成员）。
    // 键盘/鼠标回调已移交 input::InputSampler（输入并入渲染帧，无独立输入线程），
    // 本类只保留窗口尺寸回调（swapchain 重建触发）。
    static void framebufferResizeCallback(GLFWwindow* w, int width, int height);

    
    //用户输入
    engine::shared::CameraState* cameraState_ = nullptr; // 相机状态共享槽（外部挂载，非拥有；渲染只读算 MVP）
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
    VkFormat             depthFormat_ = VK_FORMAT_UNDEFINED;

    // MSAA 抗锯齿
    VkSampleCountFlagBits msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;

    // 渲染对象（第三步：RenderPass + Pipeline + 帧缓冲 + 命令 + 同步）
    static constexpr uint32_t kMaxFramesInFlight = 2;
    // HUD（S2 抽出的独立类）：可见性/统计由本类薄转发，资源与绘制都在它内部
    HudRenderer hud_;
    // 屏幕中心十字准星：与 F3 浮层**互不依赖**（独立管线/独立文件，只借用 HUD 的字体图集）。
    // 单独成类的原因：常驻 HUD 元素 vs 可开关的调试浮层，生命周期与归属都不同；
    // 将来要外置成 DLL 模组，它只依赖 Vulkan 句柄 + HudAtlas（POD）。
    engine::render::hud::CrosshairRenderer crosshair_;
    // renderPass + MSAA/深度附件 + 主管线（S4a 抽出的 PipelineSet）
    PipelineSet pipelines_;
    // 命令池/同步/UBO/描述符（S4b 抽出的 FrameInfra）
    FrameInfra infra_;
    // 世界 chunk 网格表 + P14 延迟销毁（S5 抽出的 ChunkMeshStore）
    ChunkMeshStore chunkMeshes_;
    // 刚体网格 + 位姿（S6a 抽出的 BodyMeshStore）
    BodyMeshStore bodies_;
    // 实体模型/贴图/位姿（S6b 抽出的 EntityRenderer）
    EntityRenderer entities_;
    // FPS/帧耗时统计（渲染线程自己算，喂给 HUD 显示）
    std::chrono::steady_clock::time_point fpsWindowStart_{};
    std::chrono::steady_clock::time_point lastFrameStart_{};
    double            fpsSmoothed_ = 0.0;
    double            frameMsSmoothed_ = 0.0;
    uint32_t          fpsFramesInWindow_ = 0;
    bool                 framebufferResized_ = false; // 窗口尺寸变化标志（GLFW 回调置位）
    std::vector<VkFramebuffer> framebuffers_;
    // 增量重建：每 chunk 一份网格（旧全局 buffer 逐步替换；chunk 空则无条目）。

    // ---- 延迟销毁（P14）----
    // 为什么要延迟：销毁 GPU buffer 前必须保证"引用它的命令缓冲已经执行完"。
    // 原来靠 vkDeviceWaitIdle 一次性等全部空闲（增量重建平均被拖 6.18ms），
    // 现在改成按帧槽回收：第 N 帧退休的网格放进 graveyard_[N % kMaxFramesInFlight]，
    // 下一次轮到该帧槽时（已 wait 过它的 fence）才真正销毁。
    // 此时该槽上一次提交必然完成，而其它槽更早，引用关系全部失效 → 安全。
    std::size_t            currentFrameSlot_ = 0;   // drawFrame 当前帧槽（回收用）

    // 把一个 chunk 网格标记为"待销毁"（不立即释放，见上）。
    // 回收某个帧槽里所有可销毁的网格（在该槽 fence 已 wait 之后调用）。

    // 刚体网格：key = contraptionId。与 chunkMeshes_ 同结构，但生命周期跟刚体走（打包建、解包销）。
    // 刚体世界原点（double：远离原点时 float 精度不足）。位姿更新只动这里。
    // 刚体姿态（四元数）。与 bodyPos_ 同生命周期（位姿更新只动这里，不碰缓冲）。
    // 刚体位姿：由 PoseHistory（跨线程快照表）插值后经 setBodyPoseDirect 喂入。
    // 这里只做存储，本类不再做任何插值（原先的环 + pending 相位猜测已删除）。
    // 取插值后的位姿（alpha = 现在距上一次到达 / 两次到达间隔，钳到 [0,1]）。
    // 返回 false 表示该 id 还没有位姿数据。
    bool                 renderObjectsReady_ = false;
    std::uint64_t        framesPresented_ = 0;

    // ---- 相机（MVP uniform）----
    // 相机状态由渲染线程（每帧调 InputSampler::tick）维护并写 PlayerInput；
    // 本类不再持有 camPos/yaw/pitch，updateViewProj 从 CameraState 只读。

    std::vector<engine::shared::Block> pendingRebuildBlocks_;  // 待重建快照


    glm::mat4            viewProj_{1.0f};               // 本帧的 proj*view（updateViewProj 写，pushDrawMatrices 读）

    //加入纹理链
    // 方块图集的 GPU 侧资源（S3 抽出的 TextureSet：图 + 视图 + 采样器）
    TextureSet      blockTextures_;
    VkSampler       textureSampler_ = VK_NULL_HANDLE;
    uint32_t        texWidth_ = 0;
    uint32_t        texHeight_ = 0;
    engine::assets::TextureAtlas    textureAtlas_;   // 图集：纹理名 → 格 → UV（meshing 查询用）

    // ---- 实体（带骨骼的模型）----
    // 贴图独立于方块图集：角色是整张 64×64，不参与图集打包。
    // 按路径缓存：同一张贴图只建一次，多个实体实例共用同一份 image/view/set。

    // FPS 统计（0.5 秒窗口 + 指数平滑）
};

} // namespace engine::render
