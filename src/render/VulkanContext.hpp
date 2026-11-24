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
struct Vertex {
    glm::vec3 pos;   // 方块局部坐标（-0.5..0.5 单位立方体）
    glm::vec3 color; // 顶点颜色（本阶段逐面着色，代替纹理）
    glm::vec2 uv; //  顶点纹理坐标（本阶段逐面着色，代替纹理）
};

// 顶点着色器 uniform：三个矩阵分开传（MVP 拆解）。
// model=方块世界变换（目前单位矩阵），view=相机视角，proj=投影。
// 每帧相机矩阵：CPU 预乘成 viewProj 一个矩阵再上传。
// 着色器侧 gl_Position = viewProj * bodyModel * pos（bodyModel 走 push constant）。
// 为什么合并：原来传 model/view/proj 三个 mat4（192 字节），而 model 恒为单位阵、
// 每次绘制都要在 GPU 侧乘一遍；现在 CPU 一次乘法搞定，UBO 缩到 64 字节，
// 顶点着色器的 uniform 读取从 3 个矩阵降到 1 个。
struct UniformBufferObject {
    glm::mat4 viewProj;   // = proj * view  （model 恒为单位阵，已折进来）
};

// 一个 chunk 的网格（GPU 资源 + 计数）。key = ChunkPos（ChunkPos.hpp 已提供 hash）。
// 增量重建：每个 chunk 独立持有自己的顶点/索引缓冲，脏 chunk 只重建自己那份；
// 全量接口保留为"重建全部 chunk"（首帧/世界重建用）。
// 分桶：一个 chunk 可同时持有 Solid 桶 + Cutout 桶两组网格（玻璃等），
//       重建/销毁同一个对象，生命周期原子化（不拆两个 map）。
struct ChunkMesh {
    // Solid 桶（不透明，默认桶）
    VkBuffer       vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer       indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    uint32_t       vertexCount = 0;
    uint32_t       indexCount = 0;
    // Cutout 桶（镂空/透明方块，如玻璃）：独立一组 GPU 资源 + 计数
    VkBuffer       cutoutVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cutoutVertexMemory = VK_NULL_HANDLE;
    VkBuffer       cutoutIndexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cutoutIndexMemory = VK_NULL_HANDLE;
    uint32_t       cutoutVertexCount = 0;
    uint32_t       cutoutIndexCount = 0;
};

// 一根骨骼的 GPU 网格：与 ChunkMesh 的 Solid 桶同构（实体不需要 Cutout 桶）。
struct EntityBoneGpu {
    VkBuffer       vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer       indexBuffer  = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory  = VK_NULL_HANDLE;
    uint32_t       indexCount   = 0;
};

// 一个实体的全部骨骼资源与骨骼树。
// order 是拓扑序（父一定排在子前面），每帧按它顺序累乘世界矩阵。
struct EntityGpu {
    std::vector<EntityBoneGpu> bones;
    std::vector<int>           parents;   // -1 = 根
    std::vector<glm::vec3>     pivots;    // 模型局部空间（世界单位）
    std::vector<int>           order;
    VkDescriptorSet            textureSet = VK_NULL_HANDLE; // 该实体用的贴图 descriptor
};

// 实体的每帧状态：位置 + 绕 y 轴朝向（度）+ 可见性。
// visible 属于实例而不是资源：同一个模型可以被多个实例共用，隐藏其中一个不该连带隐藏别人。
struct EntityPose {
    glm::dvec3 pos{0.0, 0.0, 0.0};
    float      yawDeg = 0.0f;
    bool       visible = true;
};

// 一张实体贴图及其 descriptor set。按贴图路径建一次，多个实体可共用同一张。
struct EntityTextureRes {
    VkImage         image   = VK_NULL_HANDLE;
    VkDeviceMemory  memory  = VK_NULL_HANDLE;
    VkImageView     view    = VK_NULL_HANDLE;
    VkSampler       sampler = VK_NULL_HANDLE;
    VkDescriptorSet set     = VK_NULL_HANDLE;
};

class VulkanContext {
public:
    void attachCameraState(engine::shared::CameraState* camera) { cameraState_ = camera; }

    // ---- HUD（游戏内调试浮层，F3 开关）----
    // 业务统计由渲染线程按 1Hz 调用：chunkCount/blockCount 要抢业务锁，别每帧查。
    void setHudStats(long long chunkCount, long long blockCount,
                     long long tickCount, double simTime);
    // 可见性由渲染线程按 F3 按下边沿切换（默认隐藏，不打扰正常游玩）。
    void setHudVisible(bool visible);
    bool hudVisible() const { return hudVisible_; }
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
    VkPipeline    cutoutPipeline() const { return cutoutPipeline_; }
    VkDescriptorSet descriptorSet() const { return descSet_; }
    bool rebuildMeshFromBlocks(const std::vector<engine::shared::Block>& blocks); // 修改为增量重建chunk
    void requestChunkRebuild(const std::vector<engine::shared::Block>& blocks,
        const std::unordered_set<engine::shared::ChunkPos>& dirtyChunks); // 请求 mesh 重建
    bool rebuildChunkMeshes(const std::vector<engine::shared::Block>& blocks, const std::unordered_set<engine::shared::ChunkPos>& dirtyChunks); // 增量重建：只重建 dirtyChunks 里的 chunk

    // ---- 刚体网格（可动方块组）----
    // 结构变化时上传/替换（打包、方块变动）；位姿每帧只改 bodyPos_/bodyRot_，不碰缓冲。
    // blocks：刚体局部坐标方块列表（网格由图集 + 方块注册表现场生成）。
    // 只能在渲染线程（或初始化期尚未启线程时）调用：VkBuffer 创建非线程安全。
    bool uploadBodyMesh(int contraptionId, const std::vector<engine::shared::Block>& blocks);
    // 位姿 = 位置 + 旋转（四元数，glm 顺序 w,x,y,z 由调用方按 {qw,qx,qy,qz} 传入）。
    // 刚体顶点是局部坐标，model matrix = translate(pos) × mat4_cast(rot)。
    // 设置刚体插值时刻（秒，模拟时间轴）；渲染循环每帧从 PlayerBodyState 取。
    void setBodyInterpTime(double sec) { bodyInterpTime_ = sec; }
    // simTime：该位姿的模拟时刻（秒）。按它入环，供 interpolatedBodyPose 查表。
    void setBodyPose(int contraptionId, double x, double y, double z,
                     float qx, float qy, float qz, float qw, double simTime);
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
    void destroyColorResources();    // MSAA 颜色图三件套（recreate 与 shutdown 共用）
    bool createRenderPass();
    bool createUniformObjects();
    bool createDepthResources();
    bool createPipeline();
    bool createFramebuffers();
    bool createCommandObjects();
    bool createSyncObjects();
    void destroyRenderObjects();
    void destroyDepthResources();
    bool createColorResources();
    bool createTextureImage();      // staging 搬运：CPU 写临时缓冲 → GPU 拷进显存
    bool createTextureImageView();  // 视图：图像 → 可采样视图
    bool createTextureSampler();    // 采样器：告诉 GPU 怎么取样（过滤/寻址）
    // 确保某张实体贴图已加载（图像/视图/采样器 + 自己的 descriptor set）。
    // 按路径去重：重复调用同一路径只建一次。返回 false 表示该贴图不可用。
    bool ensureEntityTexture(const std::string& texturePath);
    void updateViewProj(double dt); // 从 CameraState 读相机状态 → 算 viewProj（drawFrame 开头调用，只读不处理输入）
    // 推送「模型矩阵 + 相机矩阵」共 128 字节到 push constant（两个 mat4，正好是规范保证的下限）。
    // 相机矩阵以前放在单份 UBO 里，而 kMaxFramesInFlight=2 → CPU 写第 N 帧会覆盖第 N-1 帧
    // 还在读的内容，偶发一帧的相机矩阵错乱（表现为一片地形变天空色）。改走 push constant 后
    // 每个命令缓冲自带一份，不再有跨帧共享。
    void pushDrawMatrices(VkCommandBuffer cmd, const glm::mat4& bodyModel) const;
    void destroyUniformObjects();

    // ---- HUD 资源（字体纹理 + 独立管线 + 每帧顶点缓冲）----
    bool createHudResources();       // init 时创建一次：字体纹理 / 描述符 / 管线 / 顶点缓冲
    void destroyHudResources();      // shutdown 逆序释放（重复调用安全）
    bool createFontTexture();        // assets/textures/font/ascii.png → GPU 纹理（R 通道 = 覆盖率）
    bool createHudPipeline();        // 独立 pipelineLayout（只挂字体纹理）+ 关深度 + alpha 混合
    std::size_t buildHudVertices();  // 组装本帧 HUD 文本 → 顶点流（返回顶点数，供 draw）
    void drawHud(VkCommandBuffer cmd, uint32_t frameIndex); // 在 EndRenderPass 之前录制 HUD 这一趟
    

    // GLFW 回调（static 成员：可作 C 函数指针传参，同时能访问私有成员）。
    // 键盘/鼠标回调已移交 input::InputSampler（真实输入归输入线程），
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
    VkPipeline           cutoutPipeline_ = VK_NULL_HANDLE;  // Cutout 桶管线（cull NONE 双面）
    bool                 framebufferResized_ = false; // 窗口尺寸变化标志（GLFW 回调置位）
    std::vector<VkFramebuffer> framebuffers_;
    // 增量重建：每 chunk 一份网格（旧全局 buffer 逐步替换；chunk 空则无条目）。
    std::unordered_map<engine::shared::ChunkPos, ChunkMesh> chunkMeshes_;

    // ---- 延迟销毁（P14）----
    // 为什么要延迟：销毁 GPU buffer 前必须保证"引用它的命令缓冲已经执行完"。
    // 原来靠 vkDeviceWaitIdle 一次性等全部空闲（增量重建平均被拖 6.18ms），
    // 现在改成按帧槽回收：第 N 帧退休的网格放进 graveyard_[N % kMaxFramesInFlight]，
    // 下一次轮到该帧槽时（已 wait 过它的 fence）才真正销毁。
    // 此时该槽上一次提交必然完成，而其它槽更早，引用关系全部失效 → 安全。
    std::vector<ChunkMesh> graveyard_[kMaxFramesInFlight];
    std::size_t            currentFrameSlot_ = 0;   // drawFrame 当前帧槽（回收用）

    // 把一个 chunk 网格标记为"待销毁"（不立即释放，见上）。
    void retireChunkMesh(ChunkMesh&& m);
    // 回收某个帧槽里所有可销毁的网格（在该槽 fence 已 wait 之后调用）。
    void collectRetiredMeshes(std::size_t slot);

    // 刚体网格：key = contraptionId。与 chunkMeshes_ 同结构，但生命周期跟刚体走（打包建、解包销）。
    std::unordered_map<int, ChunkMesh>  bodyMeshes_;
    // 刚体世界原点（double：远离原点时 float 精度不足）。位姿更新只动这里。
    std::unordered_map<int, glm::dvec3> bodyPos_;
    // 刚体姿态（四元数）。与 bodyPos_ 同生命周期（位姿更新只动这里，不碰缓冲）。
    std::unordered_map<int, glm::quat>  bodyRot_;
    // 刚体位姿插值：物理 25Hz 推位姿、渲染 60~165Hz 画，直接覆盖会看到 40ms 一格的阶梯。
    // 4 槽环，按**模拟时刻**索引（与玩家同一条时间轴）。
    // 之前按"位姿到达的墙钟"算 alpha —— 位姿要经 物理→结果队列→业务线程→渲染队列
    // 两跳，到达时刻抖动大，还会在周期中途换对导致"跳一格"。改成按模拟时刻查表后，
    // 换对时机与渲染进度天然对齐，掉帧也能跨格插值。
    struct BodyPoseAnim {
        static constexpr int kSlots = 4;
        struct Key {
            double     t = 0.0;                        // 模拟时刻（秒）
            glm::dvec3 pos{0.0};
            glm::quat  rot{1.0f, 0.0f, 0.0f, 0.0f};
        };
        Key keys[kSlots]{};
        int count = 0;
    };
    std::unordered_map<int, BodyPoseAnim> bodyAnim_;
    // 当前渲染时刻（模拟时间轴，秒）：由渲染循环每帧从 PlayerBodyState 取。
    double bodyInterpTime_ = 0.0;
    // 取插值后的位姿（alpha = 现在距上一次到达 / 两次到达间隔，钳到 [0,1]）。
    // 返回 false 表示该 id 还没有位姿数据。
    bool interpolatedBodyPose(int contraptionId, glm::dvec3& outPos, glm::quat& outRot);
    VkCommandPool        cmdPool_ = VK_NULL_HANDLE;
    VkCommandBuffer      cmdBuffers_[kMaxFramesInFlight]{}; // 每帧槽一条（2 组轮转）
    VkSemaphore          imageAvailable_[kMaxFramesInFlight]{}; // acquire 信号量：2 组轮转
    std::vector<VkSemaphore> renderFinished_;
    VkFence              inFlightFences_[kMaxFramesInFlight]{};
    bool                 renderObjectsReady_ = false;
    std::uint64_t        framesPresented_ = 0;

    // ---- 相机（MVP uniform）----
    // 相机状态由输入线程（InputSampler）维护并写 PlayerInput；
    // 本类不再持有 camPos/yaw/pitch，updateViewProj 从 CameraState 只读。

    std::vector<engine::shared::Block> pendingRebuildBlocks_;  // 待重建快照

    std::unordered_set<engine::shared::ChunkPos> pendingDirtyChunks_; // 待重建的脏 chunk 集
    std::atomic<bool> chunkRebuildRequested_{ false };                // 增量重建请求标志

    VkBuffer             uboBuffer_ = VK_NULL_HANDLE;   // 已废弃：相机矩阵改走 push constant（见 pushDrawMatrices）
    VkDeviceMemory       uboMemory_ = VK_NULL_HANDLE;   // 同上；layout/set 保留仅为兼容旧的 binding 0
    glm::mat4            viewProj_{1.0f};               // 本帧的 proj*view（updateViewProj 写，pushDrawMatrices 读）
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
    engine::assets::TextureAtlas    textureAtlas_;   // 图集：纹理名 → 格 → UV（meshing 查询用）

    // ---- 实体（带骨骼的模型）----
    // 贴图独立于方块图集：角色是整张 64×64，不参与图集打包。
    // 按路径缓存：同一张贴图只建一次，多个实体实例共用同一份 image/view/set。
    std::unordered_map<std::string, EntityTextureRes> entityTextures_;
    std::unordered_map<int, EntityGpu>  entityMeshes_;
    std::unordered_map<int, EntityPose> entityPose_;

    // ---- HUD（游戏内调试浮层）----
    // 顶点容量：12288 顶点 = 2048 个字符 × 6 顶点。HUD 满打满算约 900 字符，留足余量。
    static constexpr std::size_t kHudVertexCapacity = 12288;
    hud::HudInfo         hudInfo_{};                  // 业务统计快照（1Hz 由渲染线程喂）
    bool                 hudVisible_ = false;         // F3 切换，默认隐藏
    bool                 hudDrawnOnce_ = false;       // 首帧绘制日志只打一次
    hud::HudTextBuilder  hudText_;                    // 逐帧重建的顶点流（CPU 侧）
    VkImage              fontImage_ = VK_NULL_HANDLE;
    VkDeviceMemory       fontImageMemory_ = VK_NULL_HANDLE;
    VkImageView          fontImageView_ = VK_NULL_HANDLE;
    VkSampler            fontSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout hudDescLayout_ = VK_NULL_HANDLE;  // 独立：binding 0 = 字体纹理
    VkDescriptorPool      hudDescPool_ = VK_NULL_HANDLE;     // 独立池：不与 3D 的 descPool_ 抢额度
    VkDescriptorSet       hudDescSet_ = VK_NULL_HANDLE;
    VkPipelineLayout      hudPipelineLayout_ = VK_NULL_HANDLE;  // 只有 push constant（屏幕尺寸）
    VkPipeline            hudPipeline_ = VK_NULL_HANDLE;
    VkBuffer          hudVertexBuffer_[kMaxFramesInFlight]{};
    VkDeviceMemory    hudVertexMemory_[kMaxFramesInFlight]{};
    void*             hudVertexMapped_[kMaxFramesInFlight]{};  // 持久映射（HOST_COHERENT，免 flush）
    // FPS 统计（0.5 秒窗口 + 指数平滑）
    std::chrono::steady_clock::time_point fpsWindowStart_{};
    std::chrono::steady_clock::time_point lastFrameStart_{};
    double            fpsSmoothed_ = 0.0;
    double            frameMsSmoothed_ = 0.0;
    uint32_t          fpsFramesInWindow_ = 0;
    // tps 统计（1Hz 采样的差分：tickCount 增量 / 墙钟增量）
    std::chrono::steady_clock::time_point tpsWindowStart_{};
    long long         tpsLastTicks_ = 0;
};

} // namespace engine::render
