// VulkanContext：GLFW 窗口 + Vulkan 实例/设备/交换链 + 渲染管线 + 每帧绘制循环。
// 函数按 init 调用顺序排列；无状态工具在 VulkanUtils，网格在 VulkanMesh。
#include "render/VulkanContext.hpp"
#include "render/VulkanMesh.hpp"
#include "render/utils/Utils.hpp"
#include "core/Log.hpp"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <glm/gtc/matrix_transform.hpp>
//导入方块类
#include "shared/chunk/ChunkPos.hpp"   
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
//相邻面剔除相关
#include "render/meshing/FaceCulling.hpp"
#include <unordered_set>
#include "registry/BlockRegistry.hpp"
#include "render/assets/ModelRegistry.hpp"
#include "render/assets/BbModelLoader.hpp"
#include "render/EntityMesh.hpp"
namespace engine::render {

// 初始化渲染上下文：窗口→实例→设备→交换链→管线→每帧对象，任一失败返回 false
bool VulkanContext::init(int width, int height, const char* title) {
    if (glfwInit() != GLFW_TRUE) {
        ENGINE_LOG_ERROR("Vulkan", "glfw 初始化失败");
        return false;
    }
    glfwInitialized_ = true;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!window_) {
        ENGINE_LOG_ERROR("Vulkan", "glfwCreateWindow 失败");
        return false;
    }
    glfwSetWindowUserPointer(window_, this);
    // 键盘/鼠标回调由 input::InputSampler::attach 注册（真实输入归输入线程）。
    glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback); // 窗口尺寸变化 → 置位重建标志
    {
        std::ostringstream os;
        os << "GLFW 窗口已创建 " << width << "x" << height;
        ENGINE_LOG_INFO("Vulkan", os.str());
    }
    //注册准星
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);//禁用鼠标光标
    // 光标基准由 input::InputSampler::attach 初始化（真实输入归输入线程）。

    if (!createInstance()) return false;
    if (!createSurface()) return false;
    if (!pickDevice()) return false;
    if (!createDevice()) return false;
    if (!createSwapchain()) return false;
    depthFormat_ = util::pickDepthFormat(physicalDevice_); // 深度格式探测（RX 6750 GRE 支持 D32）
    if (depthFormat_ == VK_FORMAT_UNDEFINED) {
        ENGINE_LOG_ERROR("Vulkan", "无支持的深度格式");
        return false;
    }
    msaaSamples_ = util::getMaxUsableSampleCount(physicalDevice_); // 先问出采样数（后面全用它）
    if (!createColorResources()) return false;                     // 先造 MSAA 图（render pass 引用它）
    if (!createRenderPass()) return false;
    if (!createDepthResources()) return false;
    if (!createFramebuffers()) return false;      // ← framebuffers 不依赖 descLayout_，可留前
    if (!createCommandObjects()) return false;
    if (!createTextureImage()) return false;
    if (!createTextureImageView()) return false;
    if (!createTextureSampler()) return false;
    if (!createUniformObjects()) return false;    // descLayout_ 就绪
    // 实体贴图不在这里加载：贴图路径由模组的实体类型定义决定，而类型表要等
    // 模组 onLoad 跑完才齐。init 之后由装配层遍历类型表调 loadEntityModel，
    // 那里再按需 ensureEntityTexture（按路径缓存，同一张只建一次）。
    if (!createPipeline()) return false;          // ← 移到这：引用 descLayout_
    if (!createHudResources()) return false;      // HUD：字体纹理 + 独立管线 + 顶点缓冲（依赖 renderPass_）
    if (!createSyncObjects()) return false;
    renderObjectsReady_ = true;
    return true;
}

// 创建 Vulkan 实例（API 版本 + 必需扩展 + 校验层）
bool VulkanContext::createInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "SandboxEngine";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "sandbox-engine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0; // 最低要求 1.0；实际驱动通常 1.2/1.3

    std::vector<const char*> enabledExts;
    for (const char* ext : util::kRequiredInstanceExtensions)
        if (util::hasInstanceExtension(ext)) enabledExts.push_back(ext);
        else {
            std::ostringstream os;
            os << "实例扩展不可用（跳过）: " << ext;
            ENGINE_LOG_WARN("Vulkan", os.str());
        }

    std::vector<const char*> enabledLayers;
    if (util::hasInstanceLayer(util::kValidationLayer)) enabledLayers.push_back(util::kValidationLayer);
    else ENGINE_LOG_WARN("Vulkan", "验证层不可用，跳过（发布环境正常）");

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExts.size());
    instanceInfo.ppEnabledExtensionNames = enabledExts.data();
    instanceInfo.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size());
    instanceInfo.ppEnabledLayerNames = enabledLayers.data();

    const VkResult r = vkCreateInstance(&instanceInfo, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        std::ostringstream os;
        os << "vkCreateInstance 失败 result=" << r;
        ENGINE_LOG_ERROR("Vulkan", os.str());
        return false;
    }
    {
        std::ostringstream os;
        os << "实例创建成功（扩展 " << enabledExts.size()
           << " 验证层 " << enabledLayers.size() << "）";
        ENGINE_LOG_INFO("Vulkan", os.str());
    }
    return true;
}

// 创建窗口表面（GLFW 窗口 ↔ 程序的桥梁）
bool VulkanContext::createSurface() {
    const VkResult r = glfwCreateWindowSurface(instance_, window_, nullptr, &surface_);
    if (r != VK_SUCCESS) {
        std::ostringstream os;
        os << "glfwCreateWindowSurface 失败 result=" << r;
        ENGINE_LOG_ERROR("Vulkan", os.str());
        return false;
    }
    ENGINE_LOG_INFO("Vulkan", "surface 创建成功（窗口↔GPU 连接点）");
    return true;
}

// 枚举物理设备，选第一个满足 图形+呈现+扩展 的显卡
bool VulkanContext::pickDevice() {
    devices_ = util::enumeratePhysicalDevices(instance_);
    if (devices_.empty()) {
        ENGINE_LOG_ERROR("Vulkan", "未找到物理设备（无显卡？）");
        return false;
    }

    for (std::size_t i = 0; i < devices_.size(); ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices_[i], &p);
        std::ostringstream os;
        os << "设备[" << i << "] " << p.deviceName
           << " | " << util::deviceTypeName(p.deviceType)
           << " | API " << VK_VERSION_MAJOR(p.apiVersion) << "."
           << VK_VERSION_MINOR(p.apiVersion) << "." << VK_VERSION_PATCH(p.apiVersion);
        ENGINE_LOG_INFO("Vulkan", os.str());
    }

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    for (VkPhysicalDevice d : devices_) {
        if (util::findGraphicsPresentFamily(d, surface_) == UINT32_MAX) continue;
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (chosen == VK_NULL_HANDLE || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) chosen = d;
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) break; // 离散卡已是最优
    }
    if (chosen == VK_NULL_HANDLE) {
        ENGINE_LOG_ERROR("Vulkan", "无支持 surface 呈现的设备");
        return false;
    }
    physicalDevice_ = chosen;
    return true;
}

// 用选中队列族创建逻辑设备，取图形/呈现队列
bool VulkanContext::createDevice() {
    graphicsQueueFamily_ = util::findGraphicsPresentFamily(physicalDevice_, surface_);
    if (graphicsQueueFamily_ == UINT32_MAX) {
        ENGINE_LOG_ERROR("Vulkan", "无 graphics+present 队列族");
        return false;
    }
    {
        std::ostringstream os;
        os << "选中队列族 #" << graphicsQueueFamily_
           << "（graphics+present，容量 " << util::queueFamilyQueueCount(physicalDevice_, graphicsQueueFamily_) << "）";
        ENGINE_LOG_INFO("Vulkan", os.str());
    }

    const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = graphicsQueueFamily_;
    queueInfo.queueCount = 1;          // 本步只要 1 条图形队列
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceFeatures features{}; // 本步不启用任何特性
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    deviceInfo.pEnabledFeatures = &features;

    const VkResult r = vkCreateDevice(physicalDevice_, &deviceInfo, nullptr, &device_);
    if (r != VK_SUCCESS) {
        std::ostringstream os;
        os << "vkCreateDevice 失败 result=" << r;
        ENGINE_LOG_ERROR("Vulkan", os.str());
        return false;
    }
    vkGetDeviceQueue(device_, graphicsQueueFamily_, 0, &graphicsQueue_);
    ENGINE_LOG_INFO("Vulkan", "逻辑设备创建成功（图形队列就绪）");
    return true;
}

// 创建交换链：呈现模式 + 图像格式 + 3 张图像（双缓冲）
bool VulkanContext::createSwapchain() {
    const VkSurfaceCapabilitiesKHR caps = util::querySurfaceCapabilities(physicalDevice_, surface_);
    swapchainFormat_ = util::chooseSwapSurfaceFormat(physicalDevice_, surface_).format;
    const VkPresentModeKHR presentMode = util::chooseSwapPresentMode(physicalDevice_, surface_);

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    swapchainExtent_ = util::chooseSwapExtent(physicalDevice_, surface_, window_);

    VkSwapchainCreateInfoKHR swapchainInfo{};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = surface_;
    swapchainInfo.minImageCount = imageCount;
    swapchainInfo.imageFormat = swapchainFormat_;   // 已由 chooseSwapSurfaceFormat 选出
    swapchainInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swapchainInfo.imageExtent = swapchainExtent_;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; // 作为颜色附件被渲染
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;     // 单队列族（我们的同族方案）
    swapchainInfo.preTransform = caps.currentTransform;              // 不额外旋转
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;    // 不透明窗口
    swapchainInfo.presentMode = presentMode;
    swapchainInfo.clipped = VK_TRUE;                                 // 被遮挡部分不绘制（性能）
    swapchainInfo.oldSwapchain = VK_NULL_HANDLE;                     // 重建时用它（本步无）

    const VkResult r = vkCreateSwapchainKHR(device_, &swapchainInfo, nullptr, &swapchain_);
    if (r != VK_SUCCESS) {
        std::ostringstream os;
        os << "vkCreateSwapchainKHR 失败 result=" << r;
        ENGINE_LOG_ERROR("Vulkan", os.str());
        return false;
    }

    uint32_t imgCount = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &imgCount, nullptr);
    swapchainImages_.resize(imgCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imgCount, swapchainImages_.data());

    swapchainImageViews_.clear();
    swapchainImageViews_.reserve(imgCount);
    for (VkImage img : swapchainImages_) {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = img;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = swapchainFormat_;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.baseMipLevel = 0;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.baseArrayLayer = 0;
        vci.subresourceRange.layerCount = 1;
        VkImageView view;
        if (vkCreateImageView(device_, &vci, nullptr, &view) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("Vulkan", "vkCreateImageView 失败");
            return false;
        }
        swapchainImageViews_.push_back(view);
    }

    {
        std::ostringstream os;
        os << "交换链创建成功：图像=" << swapchainImages_.size()
           << " 格式=" << swapchainFormat_
           << " 尺寸=" << swapchainExtent_.width << "x" << swapchainExtent_.height
           << " 呈现=" << (presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX(低延迟)" : "FIFO(vsync)");
        ENGINE_LOG_INFO("Vulkan", os.str());
    }
    return true;
}

// 销毁交换链及其图像视图（窗口重建时用）
void VulkanContext::destroySwapchain() {
    for (auto v : swapchainImageViews_) vkDestroyImageView(device_, v, nullptr);
    swapchainImageViews_.clear();
    swapchainImages_.clear();
    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

// 销毁 MSAA 颜色图三件套（顺序：视图→图像→显存；recreate 与 shutdown 共用）
void VulkanContext::destroyColorResources() {
    if (colorBufferView_) { vkDestroyImageView(device_, colorBufferView_, nullptr); colorBufferView_ = VK_NULL_HANDLE; }
    if (colorBuffer_) { vkDestroyImage(device_, colorBuffer_, nullptr); colorBuffer_ = VK_NULL_HANDLE; }
    if (colorBufferMemory_) { vkFreeMemory(device_, colorBufferMemory_, nullptr); colorBufferMemory_ = VK_NULL_HANDLE; }
}

// 窗口尺寸变化 → 销毁重建所有依赖尺寸的资源（swapchain/帧缓冲/深度/MSAA 图）
bool VulkanContext::recreateSwapchain() {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    {
        std::ostringstream os;
        os << "[重建] 触发 recreateSwapchain（新尺寸 "
           << w << "x" << h << "）";
        ENGINE_LOG_INFO("Vulkan", os.str());
    }
    if (w == 0 || h == 0) {
        // 窗口最小化/拖成 0 尺寸：extent 无效，无法重建 → 直接返回，等下一帧再试
        // （绝不能 while 阻塞等事件：渲染线程会卡死，主线程 join 也跟着卡死）
        std::ostringstream os;
        os << "[重建] 尺寸为 0，跳过本帧（等窗口恢复）";
        ENGINE_LOG_INFO("Vulkan", os.str());
        return false;
    }
    vkDeviceWaitIdle(device_); // 必须！确保所有队列上的旧资源引用已执行完
    for (auto fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr); // 帧缓冲引用旧视图，必须销毁
    framebuffers_.clear();
    destroySwapchain();        // 交换链 + 图像视图
    destroyColorResources();   // MSAA 颜色图（尺寸绑定）
    destroyDepthResources();   // 深度图（尺寸绑定）
    if (!createSwapchain()) { ENGINE_LOG_ERROR("Vulkan", "重建失败：swapchain");     return false; }
    if (!createColorResources()) { ENGINE_LOG_ERROR("Vulkan", "重建失败：MSAA 颜色图"); return false; }
    if (!createDepthResources()) { ENGINE_LOG_ERROR("Vulkan", "重建失败：深度图");      return false; }
    if (!createFramebuffers()) { ENGINE_LOG_ERROR("Vulkan", "重建失败：帧缓冲");      return false; }
    framebufferResized_ = false;
    return true;
}

// 定义渲染规则：颜色 CLEAR→画→STORE，深度 CLEAR→画→丢弃
bool VulkanContext::createRenderPass() {
    //色彩附件
    VkAttachmentDescription color{};
    color.format = swapchainFormat_;
    color.samples = msaaSamples_;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;          // 进来不知道什么布局
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;  // 停留在颜色附件布局    
    //深度附件
    VkAttachmentDescription depth{};
    depth.format = depthFormat_;
    depth.samples = msaaSamples_;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;        // 每帧清为 1.0（最远）
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;  // 深度不呈现，无需保留
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    //Resolve附件
    VkAttachmentDescription colorResolve{};
    colorResolve.format = swapchainFormat_;              // 与交换链同格式
    colorResolve.samples = VK_SAMPLE_COUNT_1_BIT;        // 最终屏幕图必须 1x
    colorResolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // 会被覆盖，不用清
    colorResolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;    // 要显示！必须保留
    colorResolve.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;  // 交给显示器

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolveAttachmentRef = {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef; // 深度附件挂进子pass
    subpass.pResolveAttachments = &resolveAttachmentRef; // 多采样抗锯齿


    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkAttachmentDescription attachments[3] = { color, depth ,colorResolve };
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 3;
    renderPassInfo.pAttachments = attachments;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dep;

    if (vkCreateRenderPass(device_, &renderPassInfo, nullptr, &renderPass_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "vkCreateRenderPass 失败");
        return false;
    }
    ENGINE_LOG_INFO("Vulkan", "RenderPass 创建成功（颜色 CLEAR→画→STORE 呈现 + 深度 CLEAR→画→丢弃）");
    return true;
}

// 创建深度图像 + 显存 + 视图（帧缓冲的深度附件）
// 图像+内存三连走通用工具（展开教学版：vkCreateImage→查需求→分配→绑定，已沉淀进 util::createImage）
bool VulkanContext::createDepthResources() {
    if (!util::createImage(device_, physicalDevice_,
            swapchainExtent_.width, swapchainExtent_.height,
            depthFormat_,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            1, msaaSamples_,
            depthImage_, depthImageMemory_)) {
        ENGINE_LOG_ERROR("Vulkan", "深度图像创建失败");
        return false;
    }

    // 视图：固定样板走通用工具（2D、深度 aspect、全 mip 全层）
    if (!util::createImageView(device_, depthImage_, depthFormat_,
            VK_IMAGE_ASPECT_DEPTH_BIT, depthImageView_)) {
        ENGINE_LOG_ERROR("Vulkan", "深度图像视图创建失败");
        return false;
    }
    {
        std::ostringstream os;
        os << "深度缓冲就绪（格式 " << util::depthFormatName(depthFormat_)
           << " " << swapchainExtent_.width << "x" << swapchainExtent_.height << "）";
        ENGINE_LOG_INFO("Vulkan", os.str());
    }
    return true;
}
//创建mass图像
bool VulkanContext::createColorResources() {
    util::createImage(device_, physicalDevice_,
        swapchainExtent_.width, swapchainExtent_.height,
        swapchainFormat_,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
        1,                                                  // 无 mipmap
        msaaSamples_,                                       // ★ 8x 采样（差异在这）
        colorBuffer_, colorBufferMemory_);
    // ② 视图（通用工具：image=colorBuffer_、format=swapchainFormat_、aspect=COLOR）
    if (!util::createImageView(device_, colorBuffer_, swapchainFormat_,
            VK_IMAGE_ASPECT_COLOR_BIT, colorBufferView_)) {
        ENGINE_LOG_ERROR("Vulkan", "MSAA 颜色视图创建失败");
        return false;
    }
    {
        std::ostringstream os;
        os << "MSAA 颜色附件就绪（"
            << swapchainExtent_.width << "x" << swapchainExtent_.height
            << " @ " << msaaSamples_ << "x 采样）";
        ENGINE_LOG_INFO("Vulkan", os.str());
    }
    return true;
}

// 创建图形管线：顶点输入 + 着色器 + 光栅化 + 深度测试 + 管线布局
bool VulkanContext::createPipeline() {
    VkShaderModule vert = util::loadShaderModule(device_, "shaders/vert.spv");
    if (vert == VK_NULL_HANDLE) return false;
    VkShaderModule frag = util::loadShaderModule(device_, "shaders/frag.spv");
    if (frag == VK_NULL_HANDLE) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = static_cast<uint32_t>(offsetof(Vertex, pos));
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset = static_cast<uint32_t>(offsetof(Vertex, color));
    attrs[2].location = 2;                                 // ② 新增
    attrs[2].binding = 0;
    attrs[2].format = VK_FORMAT_R32G32_SFLOAT;             // vec2 = 2 个 float
    attrs[2].offset = static_cast<uint32_t>(offsetof(Vertex, uv));  // ← 关键

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};   // 动态状态：创建时的值会被忽略，每帧由 vkCmdSetViewport 设置
    VkRect2D scissor{};
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dyn;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;  // 背面剔除：省一半三角形（立方体只画 3 面）
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; // 绕序与顶点索引一致
    rasterizer.depthBiasEnable = VK_FALSE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE; // 模板测试留待将来（轮廓/特效）
    depthStencil.minDepthBounds = 0.0f;
    depthStencil.maxDepthBounds = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = msaaSamples_; // 先不开 MSAA

    VkPipelineColorBlendAttachmentState blendAttach{};
    blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttach.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttach;

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descLayout_;

    // 刚体变换：每趟绘制前推一次 4x4 矩阵（静态 chunk 推单位矩阵，矩阵乘上去等于没乘）
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(glm::mat4);
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "vkCreatePipelineLayout 失败");
        vkDestroyShaderModule(device_, vert, nullptr);
        vkDestroyShaderModule(device_, frag, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil; // 深度测试/写入挂进管线
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;      // viewport/scissor 每帧设置（resize 不变形）
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 0;

    // 双管线批量创建：Solid（cull BACK）+ Cutout（cull NONE，双面——玻璃薄片两面可见）
    VkPipelineRasterizationStateCreateInfo cutoutRasterizer = rasterizer;
    cutoutRasterizer.cullMode = VK_CULL_MODE_NONE;
    VkGraphicsPipelineCreateInfo pipelines[2] = { pipelineInfo, pipelineInfo };
    pipelines[1].pRasterizationState = &cutoutRasterizer;
    VkPipeline out[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 2, pipelines, nullptr, out);
    vkDestroyShaderModule(device_, vert, nullptr);
    vkDestroyShaderModule(device_, frag, nullptr);
    if (r != VK_SUCCESS) {
        std::ostringstream os;
        os << "vkCreateGraphicsPipelines 失败 result=" << r;
        ENGINE_LOG_ERROR("Vulkan", os.str());
        return false;
    }
    pipeline_ = out[0];
    cutoutPipeline_ = out[1];
    ENGINE_LOG_INFO("Vulkan", "图形管线创建成功（Solid + Cutout 双管线）");
    return true;
}

// 每张交换链图像配一个帧缓冲（颜色 + 深度附件）
bool VulkanContext::createFramebuffers() {
    framebuffers_.reserve(swapchainImageViews_.size());
    for (std::size_t i = 0; i < swapchainImageViews_.size(); ++i) {
        VkImageView attachments[3] = { colorBufferView_, depthImageView_, swapchainImageViews_[i] }; // [颜色, 深度]
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass_;
        framebufferInfo.attachmentCount = 3;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = swapchainExtent_.width;
        framebufferInfo.height = swapchainExtent_.height;
        framebufferInfo.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &fb) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("Vulkan", "vkCreateFramebuffer 失败");
            return false;
        }
        framebuffers_.push_back(fb);
    }
    {
        std::ostringstream os;
        os << "帧缓冲创建成功（" << framebuffers_.size()
           << " 个，每交换链图像一个，含深度附件）";
        ENGINE_LOG_INFO("Vulkan", os.str());
    }
    return true;
}

// 建立通用索引缓冲
bool VulkanContext::rebuildMeshFromBlocks(const std::vector<engine::shared::Block>& blocks) {
    // 全量重建 = 增量重建的特例：推导出全部 chunk → 逐个重建（首帧/世界重建用）。
    std::unordered_set<engine::shared::ChunkPos> allChunks;
    for (const auto& b : blocks) {
        if (b.type == engine::shared::BlockType::Air || b.contraptionId != -1) continue;
        allChunks.insert(engine::shared::toChunkOf(b.pos)); // 现成工具：BlockPos → ChunkPos
    }
    if (allChunks.empty()) return true;
    return rebuildChunkMeshes(blocks, allChunks);
}

void VulkanContext::requestChunkRebuild(const std::vector<engine::shared::Block>& blocks,
    const std::unordered_set<engine::shared::ChunkPos>& dirtyChunks) {
    pendingRebuildBlocks_ = blocks;   // 快照拷贝：重建时世界数据可能已变，必须定格
    pendingDirtyChunks_ = dirtyChunks;
    chunkRebuildRequested_.store(true, std::memory_order_relaxed);
}

//销毁一个chunk的GPU资源（Solid + Cutout 两组都销毁）
static void destroyChunkMesh(VkDevice device, ChunkMesh& m) {
    // Solid 组
    if (m.vertexBuffer) vkDestroyBuffer(device, m.vertexBuffer, nullptr);
    if (m.vertexMemory) vkFreeMemory(device, m.vertexMemory, nullptr);
    if (m.indexBuffer)  vkDestroyBuffer(device, m.indexBuffer, nullptr);
    if (m.indexMemory)  vkFreeMemory(device, m.indexMemory, nullptr);
    // Cutout 组
    if (m.cutoutVertexBuffer) vkDestroyBuffer(device, m.cutoutVertexBuffer, nullptr);
    if (m.cutoutVertexMemory) vkFreeMemory(device, m.cutoutVertexMemory, nullptr);
    if (m.cutoutIndexBuffer)  vkDestroyBuffer(device, m.cutoutIndexBuffer, nullptr);
    if (m.cutoutIndexMemory)  vkFreeMemory(device, m.cutoutIndexMemory, nullptr);
    m = {}; // 全部回零（Solid + Cutout 一并）
}

//dirtychunks 更新
bool VulkanContext::rebuildChunkMeshes(
    const std::vector<engine::shared::Block>& blocks, 
    const std::unordered_set<engine::shared::ChunkPos>& dirtyChunks) {
        
    // ===== 临时计时（P14 测量用，测完删除）=====
    const auto perfT0 = std::chrono::steady_clock::now();
    // P14：原来这里有 vkDeviceWaitIdle（等整条 GPU 流水空闲，实测平均被拖 6.18ms）。
    // 现在改为"按帧槽延迟销毁"（retireChunkMesh / collectRetiredMeshes），
    // 不再需要停 GPU —— 旧 buffer 会在引用它的命令缓冲执行完之后才释放。
    const auto perfT1 = std::chrono::steady_clock::now();
    std::size_t perfCmp = 0;   // 逐 chunk 过滤时的边界比较次数
    double perfFilterMs = 0.0, perfVertMs = 0.0, perfUploadMs = 0.0;   // 三段细分
    double perfLookupMs = 0.0, perfMaskMs = 0.0, perfBlockVertMs = 0.0; // 顶点段内的三小段
    // ===== 临时计时结束 =====
    //待重建集合
    std::unordered_set<engine::shared::ChunkPos> rebuildSet = dirtyChunks;
    //solid集合
    std::unordered_set<engine::shared::BlockPos> solid;
    solid.reserve(blocks.size());
    for (const auto& b : blocks) {
        if (b.contraptionId == -1) {
            //查询注册表: 注册过且 renderType == Solid（B59：改用 renderType 判定，
            // BlockInfo 的 opaque 与它语义重叠，已删除）
            const auto* info = engine::registry::g_blockRegistry.find(
                static_cast<std::uint16_t>(b.type));
            if (info && info->renderType == engine::registry::RenderType::Solid) {
                solid.insert(b.pos);
            }
        }
    }
    //逐个chunk重建
    for (const auto& cp : rebuildSet) {
        const int bx = cp.x << 4,
            by = cp.y << 4,
            bz = cp.z << 4;
        std::vector<engine::shared::Block> chunkBlocks;
        const auto perfF0 = std::chrono::steady_clock::now();
        for (const auto& b : blocks) {
            ++perfCmp;   // 临时计时：过滤比较次数
            if (b.type == engine::shared::BlockType::Air || b.contraptionId != -1) continue;
            if (b.pos.x >= bx && b.pos.x < bx + 16 &&
                b.pos.y >= by && b.pos.y < by + 16 &&
                b.pos.z >= bz && b.pos.z < bz + 16)
                chunkBlocks.push_back(b);
        }
        const auto perfF1 = std::chrono::steady_clock::now();
        perfFilterMs += std::chrono::duration<double, std::milli>(perfF1 - perfF0).count();
        //生成chunk的顶点和索引（分桶：Solid 默认组 / Cutout 透明组）
        std::vector<Vertex> vertices;           // Solid 组顶点
        std::vector<uint32_t> indices;          // Solid 组索引
        std::vector<Vertex> cutoutVerts;        // Cutout 组顶点（玻璃等）
        std::vector<uint32_t> cutoutIndices;    // Cutout 组索引
        uint32_t vertexOffset = 0;
        uint32_t cutoutVertexOffset = 0;

        // P14-2a：渲染属性只取决于方块**类型**，而图中只有少数几种。
        // 原来每块查 3 次表（注册表 / 图集按字符串 / 模型注册表按字符串），实测 0.76 µs/块，
        // 首帧 82,944 块共烧 63 ms。这里在本次重建内按类型缓存一次，之后每块 1 次哈希。
        struct BlockRenderProps {
            bool  isCutout = false;
            glm::vec4 uvRect{0.0f};
            const engine::assets::ModelDef* model = nullptr;
        };
        std::unordered_map<std::uint16_t, BlockRenderProps> renderPropsCache;
        renderPropsCache.reserve(16);

        for (const auto& block : chunkBlocks) {
            // 细插桩（P14-2 决策用）：① 三次查表  ② 面剔除  ③ 顶点生成+拷贝
            const auto perfA0 = std::chrono::steady_clock::now();
            const glm::vec3 center{block.pos.x + 0.5f, block.pos.y + 0.5f, block.pos.z + 0.5f};
            // 类型 → 渲染属性（首次遇到该类型时查表，之后直接命中缓存）
            const auto cacheSlot =
                renderPropsCache.try_emplace(static_cast<std::uint16_t>(block.type));
            if (cacheSlot.second) {
                BlockRenderProps props;
                const auto* info = engine::registry::g_blockRegistry.find(
                    static_cast<std::uint16_t>(block.type));
                // 查注册表：renderType 决定进哪个桶（认属性不认方块身份）
                props.isCutout = info && info->renderType == engine::registry::RenderType::Cutout;
                // 纹理 → UV：注册表纹理名 → 图集格 → UV 矩形（未注册 → default 兜底格）
                props.uvRect = textureAtlas_.fallbackUV();
                if (info && !info->texture.empty()) {
                    engine::assets::AtlasSlot slot;
                    if (textureAtlas_.slotOf(info->texture, slot))
                        props.uvRect = textureAtlas_.slotUV(slot);
                }
                // 模型名查表（字符串键）
                if (info && !info->model.empty())
                    props.model = engine::assets::g_modelRegistry.find(info->model);
                cacheSlot.first->second = props;
            }
            const BlockRenderProps& props = cacheSlot.first->second;
            const bool isCutout = props.isCutout;
            auto& dstVerts   = isCutout ? cutoutVerts : vertices;
            auto& dstIndices = isCutout ? cutoutIndices : indices;
            auto& dstOffset  = isCutout ? cutoutVertexOffset : vertexOffset;
            const auto perfA1 = std::chrono::steady_clock::now();
            perfLookupMs += std::chrono::duration<double, std::milli>(perfA1 - perfA0).count();
            // 玻璃整体淡蓝；solid 不传 tint 用默认逐面色
            const std::uint8_t faceMask = optimization::meshing::computeFaceMask(block.pos, solid);
            const auto perfA2 = std::chrono::steady_clock::now();
            perfMaskMs += std::chrono::duration<double, std::milli>(perfA2 - perfA1).count();
            // 顶点生成：优先数据驱动模型（模型名注册且加载成功 → buildModelVertices；
            // 无 model / 模型缺失 → 立方体兜底，仍用注册表纹理）
            // P14-2b：直接追加进 dstVerts，不再每块新建 vector 再拷贝
            const std::size_t vertsBefore = dstVerts.size();
            if (props.model)
                mesh::buildModelVertices(dstVerts, center, *props.model, faceMask, textureAtlas_);
            if (dstVerts.size() == vertsBefore)
                mesh::buildCubeVertices(dstVerts, center, faceMask, props.uvRect);
            const uint32_t faceCount = static_cast<uint32_t>((dstVerts.size() - vertsBefore) / 4);
            for (uint32_t f = 0; f < faceCount; ++f) {
                const uint32_t b0 = dstOffset + f * 4;
                dstIndices.push_back(b0 + 0); dstIndices.push_back(b0 + 1); dstIndices.push_back(b0 + 2);
                dstIndices.push_back(b0 + 0); dstIndices.push_back(b0 + 2); dstIndices.push_back(b0 + 3);
            }
            dstOffset += static_cast<uint32_t>(dstVerts.size() - vertsBefore);
            const auto perfA3 = std::chrono::steady_clock::now();
            perfBlockVertMs += std::chrono::duration<double, std::milli>(perfA3 - perfA2).count();
        }
        //无chunk 删除旧网格, 继续下一个
        const auto perfV1 = std::chrono::steady_clock::now();
        perfVertMs += std::chrono::duration<double, std::milli>(perfV1 - perfF1).count();
        auto it =  chunkMeshes_.find(cp);
        if (vertices.empty() && cutoutVerts.empty()) {
            if (it != chunkMeshes_.end()) {
                retireChunkMesh(std::move(it->second));   // P14：延迟销毁，不等 GPU
                chunkMeshes_.erase(it);
            }
            continue;
        }
        //有网格
        if (it != chunkMeshes_.end()) {
            retireChunkMesh(std::move(it->second));       // P14：延迟销毁，不等 GPU
            chunkMeshes_.erase(it);
        }
        ChunkMesh m;
        // Solid 组 buffer
        if (!util::createBuffer(device_, physicalDevice_, sizeof(Vertex) * vertices.size(),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            m.vertexBuffer, m.vertexMemory)) return false;
        util::mapAndCopy(device_, m.vertexMemory, sizeof(Vertex) * vertices.size(), vertices.data());
        if (!util::createBuffer(device_, physicalDevice_, sizeof(uint32_t) * indices.size(),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            m.indexBuffer, m.indexMemory)) return false;
        util::mapAndCopy(device_, m.indexMemory, sizeof(uint32_t) * indices.size(), indices.data());
        m.vertexCount = static_cast<uint32_t>(vertices.size());
        m.indexCount = static_cast<uint32_t>(indices.size());
        // Cutout 组 buffer（无 cutout 面则跳过，保持 NULL 句柄）
        if (!cutoutVerts.empty()) {
            if (!util::createBuffer(device_, physicalDevice_, sizeof(Vertex) * cutoutVerts.size(),
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                m.cutoutVertexBuffer, m.cutoutVertexMemory)) return false;
            util::mapAndCopy(device_, m.cutoutVertexMemory, sizeof(Vertex) * cutoutVerts.size(), cutoutVerts.data());
            if (!util::createBuffer(device_, physicalDevice_, sizeof(uint32_t) * cutoutIndices.size(),
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                m.cutoutIndexBuffer, m.cutoutIndexMemory)) return false;
            util::mapAndCopy(device_, m.cutoutIndexMemory, sizeof(uint32_t) * cutoutIndices.size(), cutoutIndices.data());
            m.cutoutVertexCount = static_cast<uint32_t>(cutoutVerts.size());
            m.cutoutIndexCount = static_cast<uint32_t>(cutoutIndices.size());
        }
        chunkMeshes_[cp] = m;
        const auto perfU1 = std::chrono::steady_clock::now();
        perfUploadMs += std::chrono::duration<double, std::milli>(perfU1 - perfV1).count();

        //重建日志（面数 = Solid + Cutout）
        {
            std::ostringstream os;
            os << "重建 chunk=(" << cp.x << "," << cp.y << "," << cp.z
                << ") 面=" << (m.indexCount + m.cutoutIndexCount) / 6
                << " 顶点=" << (m.vertexCount + m.cutoutVertexCount);
            ENGINE_LOG_INFO("Vulkan", os.str());
        }
    }
    // ===== 临时计时（P14 测量用，测完删除）=====
    {
        const auto perfT2 = std::chrono::steady_clock::now();
        const double waitMs = std::chrono::duration<double, std::milli>(perfT1 - perfT0).count();
        const double totalMs = std::chrono::duration<double, std::milli>(perfT2 - perfT0).count();
        std::ostringstream os;
        os << "[perfP14] chunks=" << rebuildSet.size()
           << " blocks=" << blocks.size()
           << " cmp=" << perfCmp
           << " waitIdle=" << waitMs << "ms"
           << " 过滤=" << perfFilterMs << "ms"
           << " 顶点=" << perfVertMs << "ms"
           << "[查表=" << perfLookupMs
           << " 面剔除=" << perfMaskMs
           << " 生成=" << perfBlockVertMs << "]"
           << " 上传=" << perfUploadMs << "ms"
           << " total=" << totalMs << "ms";
        ENGINE_LOG_INFO("Vulkan", os.str());
    }
    // ===== 临时计时结束 =====
    return true;
}

// ---- 延迟销毁（P14）----
// 把网格挂到"当前帧槽"的墓地：等下一次轮回到该帧槽（那时它的 fence 已经 wait 过），
// 引用这个网格的提交必然全部执行完，才真正 vkDestroyBuffer。
//
// 安全性推导（kMaxFramesInFlight = 2）：
//   在第 F 帧（槽 S）退休的网格，最后一次被引用是 F-1 帧（槽 1-S）；
//   第 F+2 帧重新轮到槽 S 时，S 的 fence（F 帧）刚 wait 完，而 1-S 的 fence（F-1 帧）
//   在第 F+1 帧已经 wait 过 —— 两者都完成，销毁安全。
void VulkanContext::retireChunkMesh(ChunkMesh&& m) {
    graveyard_[currentFrameSlot_ % kMaxFramesInFlight].push_back(std::move(m));
}

void VulkanContext::collectRetiredMeshes(std::size_t slot) {
    auto& bucket = graveyard_[slot % kMaxFramesInFlight];
    for (auto& m : bucket) destroyChunkMesh(device_, m);
    bucket.clear();
}

// ---- 刚体网格（可动方块组）----

bool VulkanContext::uploadBodyMesh(int contraptionId, const std::vector<engine::shared::Block>& blocks) {
    // 同 id 已有网格：结构变了，先销毁旧缓冲再建新的
    auto it = bodyMeshes_.find(contraptionId);
    if (it != bodyMeshes_.end()) {
        destroyChunkMesh(device_, it->second);
        bodyMeshes_.erase(it);
    }

    // 局部坐标生成顶点（不查世界邻居）；图集与注册表由本层提供
    const mesh::BodyMeshData data = mesh::buildBodyVertices(blocks, textureAtlas_);
    if (data.vertices.empty() || data.indices.empty())
        return true;   // 空刚体：不建缓冲

    ChunkMesh m;
    if (!util::createBuffer(device_, physicalDevice_, sizeof(Vertex) * data.vertices.size(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        m.vertexBuffer, m.vertexMemory)) return false;
    util::mapAndCopy(device_, m.vertexMemory, sizeof(Vertex) * data.vertices.size(), data.vertices.data());

    if (!util::createBuffer(device_, physicalDevice_, sizeof(std::uint32_t) * data.indices.size(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        m.indexBuffer, m.indexMemory)) return false;
    util::mapAndCopy(device_, m.indexMemory, sizeof(std::uint32_t) * data.indices.size(), data.indices.data());

    m.vertexCount = static_cast<std::uint32_t>(data.vertices.size());
    m.indexCount  = static_cast<std::uint32_t>(data.indices.size());
    bodyMeshes_[contraptionId] = m;

    ENGINE_LOG_INFO("Vulkan", "刚体网格 id=" + std::to_string(contraptionId)
        + " 顶点=" + std::to_string(m.vertexCount)
        + " 索引=" + std::to_string(m.indexCount));
    return true;
}

void VulkanContext::setBodyPose(int contraptionId, double x, double y, double z,
                                float qx, float qy, float qz, float qw) {
    bodyPos_[contraptionId] = glm::dvec3{x, y, z};
    // glm::quat 的构造顺序是 (w, x, y, z) —— 这里显式换序，避免 (x,y,z,w) 误用。
    bodyRot_[contraptionId] = glm::quat{qw, qx, qy, qz};
}

void VulkanContext::destroyBodyMesh(int contraptionId) {
    auto it = bodyMeshes_.find(contraptionId);
    if (it != bodyMeshes_.end()) {
        destroyChunkMesh(device_, it->second);
        bodyMeshes_.erase(it);
    }
    bodyPos_.erase(contraptionId);
    bodyRot_.erase(contraptionId);
}

// ---- 实体模型（带骨骼，逐骨骼 draw）----

bool VulkanContext::loadEntityModel(int entityId, const std::string& bbmodelPath,
                                    const std::string& texturePath) {
    engine::assets::EntityModel model;
    if (!engine::assets::loadBbModel(bbmodelPath, model))
        return false;

    // 贴图路径由实体类型定义给出；空串表示"由模型文件自己寻址"。
    // 模型自寻址（bbmodel 内嵌 base64 导出）还没做，因此空串时直接不加载贴图，
    // 该实体不绘制——渲染层不再假设"默认玩家贴图"这种东西。
    const std::string texPath = texturePath;

    const mesh::EntityMeshData data = mesh::buildEntityVertices(model);
    if (data.bones.size() != model.bones.size())
        return false;

    EntityGpu gpu;
    gpu.bones.resize(model.bones.size());
    gpu.parents.resize(model.bones.size(), -1);
    gpu.pivots.resize(model.bones.size(), glm::vec3(0.0f));

    for (std::size_t i = 0; i < data.bones.size(); ++i) {
        const auto& bm = data.bones[i];
        gpu.parents[i] = model.bones[i].parent;
        gpu.pivots[i] = model.bones[i].pivot * 0.0625f;   // 16 单位 → 世界单位
        if (bm.vertices.empty() || bm.indices.empty()) continue;

        EntityBoneGpu& g = gpu.bones[i];
        if (!util::createBuffer(device_, physicalDevice_,
                sizeof(Vertex) * bm.vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                g.vertexBuffer, g.vertexMemory)) return false;
        util::mapAndCopy(device_, g.vertexMemory, sizeof(Vertex) * bm.vertices.size(), bm.vertices.data());

        if (!util::createBuffer(device_, physicalDevice_,
                sizeof(std::uint32_t) * bm.indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                g.indexBuffer, g.indexMemory)) return false;
        util::mapAndCopy(device_, g.indexMemory, sizeof(std::uint32_t) * bm.indices.size(), bm.indices.data());

        g.indexCount = static_cast<std::uint32_t>(bm.indices.size());
    }

    // 拓扑序：父必须排在子前面，每帧按这个顺序从根往下累乘世界矩阵
    std::vector<std::vector<int>> children(model.bones.size());
    std::vector<int> indeg(model.bones.size(), 0);
    for (std::size_t i = 0; i < model.bones.size(); ++i) {
        const int p = gpu.parents[i];
        if (p >= 0) { children[p].push_back(static_cast<int>(i)); indeg[i] = 1; }
    }
    for (std::size_t i = 0; i < model.bones.size(); ++i)
        if (indeg[i] == 0) gpu.order.push_back(static_cast<int>(i));
    for (std::size_t k = 0; k < gpu.order.size(); ++k)
        for (int c : children[gpu.order[k]]) gpu.order.push_back(c);

    // 贴图：加载失败则 textureSet 保持空，绘制趟跳过这个实体（不影响方块）
    if (ensureEntityTexture(texPath)) {
        gpu.textureSet = entityTextures_[texPath].set;
    } else {
        ENGINE_LOG_WARN("Vulkan", "实体 id=" + std::to_string(entityId) + " 无可用贴图，将不绘制");
    }

    const std::size_t boneCount = gpu.bones.size();
    entityMeshes_[entityId] = std::move(gpu);
    entityPose_[entityId] = EntityPose{};

    ENGINE_LOG_INFO("Vulkan", "实体网格 id=" + std::to_string(entityId)
        + " 骨骼=" + std::to_string(boneCount));
    return true;
}

void VulkanContext::setEntityPose(int entityId, double x, double y, double z, float yawDeg) {
    EntityPose& p = entityPose_[entityId];
    p.pos = glm::dvec3{x, y, z};
    p.yawDeg = yawDeg;
}

// 可见性写在实例状态（entityPose_）而不是资源（entityMeshes_）：
// 同一份网格可能被多个实例共用，隐藏其中一个不该连带隐藏别人。
void VulkanContext::setEntityVisible(int entityId, bool visible) {
    auto it = entityPose_.find(entityId);
    if (it != entityPose_.end()) it->second.visible = visible;
}

void VulkanContext::destroyEntityModel(int entityId) {
    auto it = entityMeshes_.find(entityId);
    if (it != entityMeshes_.end()) {
        for (auto& g : it->second.bones) {
            if (g.vertexBuffer) vkDestroyBuffer(device_, g.vertexBuffer, nullptr);
            if (g.vertexMemory) vkFreeMemory(device_, g.vertexMemory, nullptr);
            if (g.indexBuffer)  vkDestroyBuffer(device_, g.indexBuffer, nullptr);
            if (g.indexMemory)  vkFreeMemory(device_, g.indexMemory, nullptr);
        }
        entityMeshes_.erase(it);
    }
    entityPose_.erase(entityId);
}


// 创建命令池 + 每帧命令缓冲（2 条轮转）
bool VulkanContext::createCommandObjects() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // 允许每帧 reset
    poolInfo.queueFamilyIndex = graphicsQueueFamily_;
    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &cmdPool_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "vkCreateCommandPool 失败");
        return false;
    }
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = cmdPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kMaxFramesInFlight;
    if (vkAllocateCommandBuffers(device_, &allocInfo, cmdBuffers_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "vkAllocateCommandBuffers 失败");
        return false;
    }
    ENGINE_LOG_INFO("Vulkan", "命令池 + 命令缓冲就绪（每帧槽一条，2 条轮转）");
    return true;
}

// 纹理图像：staging 搬运（构建图集 → 上传显存 → 转 SHADER_READ_ONLY）
// 图集 = 一张大纹理装所有方块小纹理（每格 16²，边长由声明条目数算出）
bool VulkanContext::createTextureImage() {
    if (!textureAtlas_.build()) {   // 读 assets/atlases/blocks.json → 拼图集（CPU 内存 RGBA）
        ENGINE_LOG_ERROR("Vulkan", "图集构建失败");
        return false;
    }
    const auto& pixels = textureAtlas_.rgba();
    const std::uint32_t w = textureAtlas_.size();
    const std::uint32_t h = textureAtlas_.size();
    texWidth_ = w;
    texHeight_ = h;
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(w) * h * engine::assets::kAtlasChannels; // RGBA×4字节

    if (!util::createImage(device_, physicalDevice_, w, h, VK_FORMAT_R8G8B8A8_SRGB,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        1, VK_SAMPLE_COUNT_1_BIT, textureImage_, textureImageMemory_)) {
        return false;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    if (!util::createBuffer(device_, physicalDevice_, imageSize,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        staging, stagingMem)) {
        return false;
    }
    void* data = nullptr;
    vkMapMemory(device_, stagingMem, 0, imageSize, 0, &data);
    std::memcpy(data, pixels.data(), static_cast<std::size_t>(imageSize));  // ← 图集像素进 staging
    vkUnmapMemory(device_, stagingMem);

    util::transitionImageLayout(device_, graphicsQueue_, cmdPool_, textureImage_,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    util::copyBufferToImage(device_, graphicsQueue_, cmdPool_, staging, textureImage_, w, h);
    util::transitionImageLayout(device_, graphicsQueue_, cmdPool_, textureImage_,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);
    return true;
}

 

// 纹理图像 → 可采样视图
bool VulkanContext::createTextureImageView() {
    // 视图：固定样板走通用工具（2D、颜色 aspect、全 mip 全层）
    if (!util::createImageView(device_, textureImage_, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_ASPECT_COLOR_BIT, textureImageView_)) {
        ENGINE_LOG_ERROR("Vulkan", "纹理视图创建失败");
        return false;
    }
    return true;
}
// 创建采样器（线性过滤 + REPEAT 寻址）
bool VulkanContext::createTextureSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;      // 放大最近邻：16² 像素风硬块（线性会糊）
    samplerInfo.minFilter = VK_FILTER_NEAREST;      // 缩小最近邻
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;  // 图集防相邻格泄漏
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;   // 1 级占位
    samplerInfo.maxLod = 1.0f;
    if (vkCreateSampler(device_, &samplerInfo, nullptr, &textureSampler_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "纹理采样器创建失败");
        return false;
    }
    return true;
}

// 确保某张实体贴图已加载：图像 + 视图 + 最近邻采样器 + 自己的 descriptor set。
// 按路径缓存——同一张贴图被多个实体类型引用时只建一次。
bool VulkanContext::ensureEntityTexture(const std::string& path) {
    // 空路径 = 类型定义没给贴图，也没做模型自寻址：直接判无贴图，不占描述符额度。
    if (path.empty()) return false;

    auto it = entityTextures_.find(path);
    if (it != entityTextures_.end()) return it->second.set != VK_NULL_HANDLE;

    EntityTextureRes res;

    // 先占一个 descriptor set：池里按 kMaxEntityTextures 预留了额度
    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = descPool_;
    setAlloc.descriptorSetCount = 1;
    setAlloc.pSetLayouts = &descLayout_;
    if (vkAllocateDescriptorSets(device_, &setAlloc, &res.set) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "实体贴图描述符集分配失败：" + path);
        return false;
    }

    int w = 0, h = 0, ch = 0;
    stbi_uc* src = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
    if (!src) {
        ENGINE_LOG_WARN("Vulkan", "实体贴图读不到：" + path + "（引用它的实体不绘制）");
        return false;
    }

    const VkDeviceSize size = static_cast<VkDeviceSize>(w) * static_cast<VkDeviceSize>(h) * 4;
    if (!util::createImage(device_, physicalDevice_,
            static_cast<uint32_t>(w), static_cast<uint32_t>(h), VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            1, VK_SAMPLE_COUNT_1_BIT, res.image, res.memory)) {
        stbi_image_free(src);
        ENGINE_LOG_ERROR("Vulkan", "实体贴图图像创建失败：" + path);
        return false;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    if (!util::createBuffer(device_, physicalDevice_, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging, stagingMem)) {
        stbi_image_free(src);
        return false;
    }
    void* data = nullptr;
    vkMapMemory(device_, stagingMem, 0, size, 0, &data);
    std::memcpy(data, src, static_cast<std::size_t>(size));
    vkUnmapMemory(device_, stagingMem);
    stbi_image_free(src);

    util::transitionImageLayout(device_, graphicsQueue_, cmdPool_, res.image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    util::copyBufferToImage(device_, graphicsQueue_, cmdPool_, staging, res.image,
        static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    util::transitionImageLayout(device_, graphicsQueue_, cmdPool_, res.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);

    if (!util::createImageView(device_, res.image, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_ASPECT_COLOR_BIT, res.view)) {
        ENGINE_LOG_ERROR("Vulkan", "实体贴图视图创建失败：" + path);
        return false;
    }

    // 像素画必须最近邻；CLAMP 防边缘泄漏
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.maxLod = 1.0f;
    if (vkCreateSampler(device_, &si, nullptr, &res.sampler) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "实体采样器创建失败：" + path);
        return false;
    }

    // binding 0 = 与方块共用的同一个 UBO；binding 1 = 这张贴图
    VkDescriptorBufferInfo binfo{ uboBuffer_, 0, sizeof(UniformBufferObject) };
    VkDescriptorImageInfo  ii{ res.sampler, res.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr[2]{};
    wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[0].dstSet = res.set;
    wr[0].dstBinding = 0;
    wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wr[0].pBufferInfo = &binfo;
    wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[1].dstSet = res.set;
    wr[1].dstBinding = 1;
    wr[1].descriptorCount = 1;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr[1].pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 2, wr, 0, nullptr);

    entityTextures_[path] = res;
    ENGINE_LOG_INFO("Vulkan", "实体贴图就绪：" + path + "（" + std::to_string(w) + "x" + std::to_string(h) + "）");
    return true;
}

// ---- GLFW 输入回调（static 成员定义；经 user pointer 转发到 this）----
// 描述符集：UBO（binding 0）+ 纹理 sampler（binding 1）
bool VulkanContext::createUniformObjects() {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = sizeof(UniformBufferObject);
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bi, nullptr, &uboBuffer_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "UBO vkCreateBuffer 失败");
        return false;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, uboBuffer_, &req);
    uint32_t memIdx = util::findMemoryType(physicalDevice_, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memIdx == UINT32_MAX) {
        ENGINE_LOG_ERROR("Vulkan", "UBO 无 host 可见内存类型");
        return false;
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memIdx;
    if (vkAllocateMemory(device_, &ai, nullptr, &uboMemory_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "UBO vkAllocateMemory 失败");
        return false;
    }
    vkBindBufferMemory(device_, uboBuffer_, uboMemory_, 0);

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT; // 只有顶点着色器用 MVP
    VkDescriptorSetLayoutBinding samplerBinding{};     // ← 新增
    samplerBinding.binding = 1;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding bindings[2] = { binding, samplerBinding };
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descLayout_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "vkCreateDescriptorSetLayout 失败");
        return false;
    }

    // 一套 set 的组成：binding 0 = UBO，binding 1 = 贴图。
    // 额度 = 方块图集 1 套 + 实体贴图各 1 套。实体贴图数量上限 6 张，
    // 超出的 ensureEntityTexture 会在分配 set 时报错返回 false（不是崩溃）。
    constexpr uint32_t kMaxEntityTextures = 6;
    VkDescriptorPoolSize poolSizes[2] = {
    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 + kMaxEntityTextures},
    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 + kMaxEntityTextures},   // 图集 + 各实体贴图
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1 + kMaxEntityTextures;         // 方块一套 + 每个实体贴图一套
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descPool_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "vkCreateDescriptorPool 失败");
        return false;
    }

    // 方块图集那一套在这里一次性分配；实体贴图的 set 由 ensureEntityTexture 按需分配
    VkDescriptorSetLayout setLayouts[1] = { descLayout_ };
    VkDescriptorSetAllocateInfo setAllocInfo{};
    setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAllocInfo.descriptorPool = descPool_;
    setAllocInfo.descriptorSetCount = 1;
    setAllocInfo.pSetLayouts = setLayouts;
    VkDescriptorSet sets[1]{};
    if (vkAllocateDescriptorSets(device_, &setAllocInfo, sets) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "vkAllocateDescriptorSets 失败");
        return false;
    }
    descSet_ = sets[0];        // 方块图集那套

    VkDescriptorBufferInfo binfo{uboBuffer_, 0, sizeof(UniformBufferObject)};
    VkDescriptorImageInfo iinfo{ textureSampler_, textureImageView_,
    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };       // ← 新增：采样器+视图
    VkWriteDescriptorSet write[2]{};
    write[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write[0].dstSet = descSet_; 
    write[0].dstBinding = 0;   // 现有 UBO 写
    write[0].descriptorCount = 1; 
    write[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write[0].pBufferInfo = &binfo;
    write[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write[1].dstSet = descSet_; 
    write[1].dstBinding = 1;   // ← 新增 sampler 写
    write[1].descriptorCount = 1; 
    write[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write[1].pImageInfo = &iinfo;
    vkUpdateDescriptorSets(device_, 2, write, 0, nullptr);

    ENGINE_LOG_INFO("Vulkan", "MVP uniform 就绪（descriptor set + UBO，相机可操作）");
    return true;
}

// 同步对象：acquire/renderFinished 信号量 + 栅栏（每帧轮转）
bool VulkanContext::createSyncObjects() {
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // 初始已 signaled：第一帧 wait 直接过

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailable_[i]) != VK_SUCCESS ||
            vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("Vulkan", "同步对象创建失败");
            return false;
        }
    }
    renderFinished_.resize(swapchainImages_.size(), VK_NULL_HANDLE);
    for (auto& sem : renderFinished_) {
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &sem) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("Vulkan", "renderFinished 信号量创建失败");
            return false;
        }
    }
    ENGINE_LOG_INFO("Vulkan", "同步对象就绪（acquire×2 轮转 + renderFinished×图像数 + 栅栏×2）");
    return true;
}

// 每帧从 CameraState 读相机状态 → 计算 MVP → 写 UBO。
// 相机状态由输入线程（InputSampler）维护；本函数只读，不再处理输入。
void VulkanContext::updateViewProj(double /*dt*/) {
    double px = 0.0, py = 0.0, pz = 3.5, pyaw = 0.0, ppitch = 0.0;
    if (cameraState_) {
        px = cameraState_->camX.load(std::memory_order_relaxed);
        py = cameraState_->camY.load(std::memory_order_relaxed);
        pz = cameraState_->camZ.load(std::memory_order_relaxed);
        pyaw = cameraState_->yaw.load(std::memory_order_relaxed);
        ppitch = cameraState_->pitch.load(std::memory_order_relaxed);
    }
    const float yaw   = static_cast<float>(pyaw);
    const float pitch = static_cast<float>(ppitch);
    const glm::vec3 camPos(static_cast<float>(px),
                           static_cast<float>(py),
                           static_cast<float>(pz));

    // 纵横比：启动竞态时交换链尺寸可能未就绪（height=0 → 除零 NaN 触发 glm assert）
    const float aspect = (swapchainExtent_.height > 0)
        ? static_cast<float>(swapchainExtent_.width) / static_cast<float>(swapchainExtent_.height)
        : 1.0f;
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 100.0f);
    proj[1][1] *= -1.0f; // Vulkan 的 NDC Y 轴向下；glm 按 OpenGL 设计，需翻转

    const glm::vec3 lookDir(
        std::sin(yaw) * std::cos(pitch),
        std::sin(pitch),
        -std::cos(yaw) * std::cos(pitch)); // -Z 前方，方块默认在视野内
    const glm::mat4 view = glm::lookAt(camPos, camPos + lookDir, glm::vec3(0.0f, 1.0f, 0.0f));

    UniformBufferObject ubo{};
    // 预乘：proj * view（model 恒为单位阵，不再上传也不再在着色器里乘）。
    // 具体模型变换（刚体/骨骼）仍走 push constant，逐 draw 变化。
    ubo.viewProj = proj * view;
    void* data = nullptr;
    vkMapMemory(device_, uboMemory_, 0, sizeof(ubo), 0, &data);
    std::memcpy(data, &ubo, sizeof(ubo));
    vkUnmapMemory(device_, uboMemory_);
}

// 每帧绘制：acquire → 录制 → submit → present
bool VulkanContext::drawFrame() {
    if (!renderObjectsReady_) return false;
    updateViewProj(0.006); // 帧间隔 6.06ms（165Hz 目标；FIFO vsync 拉齐到显示器刷新）

    // HUD 帧率统计：帧开始打点。FPS 用 0.5 秒窗口算（避免逐帧抖动），帧间隔做指数平滑。
    {
        const auto now = std::chrono::steady_clock::now();
        if (lastFrameStart_.time_since_epoch().count() == 0) {   // 首帧：只初始化基准
            lastFrameStart_ = now;
            fpsWindowStart_ = now;
        }
        const double dtMs =
            std::chrono::duration<double, std::milli>(now - lastFrameStart_).count();
        if (dtMs > 0.0 && dtMs < 1000.0) {
            frameMsSmoothed_ = (frameMsSmoothed_ == 0.0) ? dtMs : frameMsSmoothed_ * 0.9 + dtMs * 0.1;
        }
        lastFrameStart_ = now;
        ++fpsFramesInWindow_;
        const double windowSec = std::chrono::duration<double>(now - fpsWindowStart_).count();
        if (windowSec >= 0.5) {
            fpsSmoothed_ = static_cast<double>(fpsFramesInWindow_) / windowSec;
            fpsFramesInWindow_ = 0;
            fpsWindowStart_ = now;
        }
    }

    const uint32_t frame =
        static_cast<uint32_t>(framesPresented_) % kMaxFramesInFlight;
    vkWaitForFences(device_, 1, &inFlightFences_[frame], VK_TRUE, UINT64_MAX);
    // 注意：vkResetFences 必须在 acquire 成功之后！
    // acquire 失败(OUT_OF_DATE)时 fence 保持 signaled → 下一帧 wait 直接通过
    // 若先 reset，失败帧没有 submit 触发 fence → 下一帧 wait 无限阻塞 → 渲染线程卡死

    // P14：这个帧槽的 fence 刚 wait 完 → 上一轮挂在这里的旧网格可以真正销毁了。
    currentFrameSlot_ = frame;
    collectRetiredMeshes(frame);

    // 增量重建请求：只重建脏 chunk（∪6 邻居由 rebuildChunkMeshes 内部展开）
    if (chunkRebuildRequested_.exchange(false, std::memory_order_relaxed)) {
        rebuildChunkMeshes(pendingRebuildBlocks_, pendingDirtyChunks_);
        pendingRebuildBlocks_.clear();
        pendingDirtyChunks_.clear();
    }

    uint32_t imageIndex = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                       imageAvailable_[frame], VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();   // 返回 false 也无妨，本帧反正跳过
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        std::ostringstream os;
        os << "vkAcquireNextImageKHR 失败 result=" << r;
        ENGINE_LOG_ERROR("Vulkan", os.str());
        return false;
    }
    vkResetFences(device_, 1, &inFlightFences_[frame]); // acquire 成功后才重置

    if (chunkMeshes_.empty()) {
        return true; // 没有任何 chunk 网格 → 跳过本帧
    }

    vkResetCommandBuffer(cmdBuffers_[frame], 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(cmdBuffers_[frame], &beginInfo) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "vkBeginCommandBuffer 失败");
        return false;
    }
    VkRenderPassBeginInfo renderPassBeginInfo{};
    renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassBeginInfo.renderPass = renderPass_;
    renderPassBeginInfo.framebuffer = framebuffers_[imageIndex];
    renderPassBeginInfo.renderArea = {{0, 0}, swapchainExtent_};
    VkClearValue clears[2]{};
    clears[0].color = {{0.02f, 0.02f, 0.05f, 1.0f}}; // 颜色清屏：深蓝黑
    clears[1].depthStencil = {1.0f, 0};             // 深度清屏：最远（1.0），stencil 0
    renderPassBeginInfo.clearValueCount = 2;
    renderPassBeginInfo.pClearValues = clears;
    vkCmdBeginRenderPass(cmdBuffers_[frame], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
    // 动态 viewport/scissor：用当前交换链尺寸（resize 后自动适配，管线无需重建）
    VkViewport vp{};
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width  = static_cast<float>(swapchainExtent_.width);
    vp.height = static_cast<float>(swapchainExtent_.height);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmdBuffers_[frame], 0, 1, &vp);
    VkRect2D scissor{{0, 0}, swapchainExtent_};
    vkCmdSetScissor(cmdBuffers_[frame], 0, 1, &scissor);
    // 描述符（UBO+纹理）：双管线共用同一 layout/descSet，绑定一次即可
    vkCmdBindDescriptorSets(cmdBuffers_[frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &descSet_, 0, nullptr);

    // 刚体变换 push constant：静态两趟各推一次单位矩阵。
    // Cutout 趟必须重推——push constant 是命令缓冲状态，不重推会沿用上一趟的值。
    const glm::mat4 identityMatrix(1.0f);

    // 第一趟：Solid 桶（不透明先画，写深度建立遮挡基准）
    vkCmdBindPipeline(cmdBuffers_[frame], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdPushConstants(cmdBuffers_[frame], pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(glm::mat4), &identityMatrix);
    for (const auto& [cp, m] : chunkMeshes_) {
        if (m.vertexCount == 0) continue;   // 无 Solid 面跳过
        VkDeviceSize vbOffset = 0;
        vkCmdBindVertexBuffers(cmdBuffers_[frame], 0, 1, &m.vertexBuffer, &vbOffset);
        vkCmdBindIndexBuffer(cmdBuffers_[frame], m.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmdBuffers_[frame], m.indexCount, 1, 0, 0, 0);
    }

    // 刚体趟：每刚体一次 draw，push constant 携带它的世界平移。
    // ubo.view 已经含相机平移（lookAt(camPos, ...)），所以这里必须推"世界坐标"；
    // 再减一次相机位置等于把偏移算两遍，刚体会飞到视野外。
    // 精度：bodyPos 是 double，这里转 float；原型阶段世界只有 ±64 格，够用。
    // 真正远离原点时要走"局部坐标 + 每对象偏移"，见 架构决策-精度方案.md。
    {
        // 首帧诊断：确认刚体趟真的拿到了网格与位姿
        static bool bodyDrawLogged = false;
        if (!bodyDrawLogged) {
            bodyDrawLogged = true;
            std::ostringstream os;
            os << "刚体趟首帧 数量=" << bodyMeshes_.size();
            const auto pit0 = bodyPos_.find(0);
            if (pit0 != bodyPos_.end())
                os << " 刚体0=(" << pit0->second.x << "," << pit0->second.y << "," << pit0->second.z << ")";
            ENGINE_LOG_INFO("Vulkan", os.str());
        }
        for (const auto& [id, m] : bodyMeshes_) {
            if (m.vertexCount == 0) continue;
            const auto pit = bodyPos_.find(id);
            if (pit == bodyPos_.end()) continue;
            const glm::vec3 worldPos{
                static_cast<float>(pit->second.x),
                static_cast<float>(pit->second.y),
                static_cast<float>(pit->second.z)};
            // 顶点在局部坐标（buildBodyVertices），所以旋转在 model matrix 里做：
            // T(pos) × R(quat)。没有旋转记录时用单位四元数（等价于原来的纯平移）。
            const auto rit = bodyRot_.find(id);
            const glm::quat bodyRot =
                (rit != bodyRot_.end()) ? rit->second : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
            const glm::mat4 bodyModel =
                glm::translate(glm::mat4(1.0f), worldPos) * glm::mat4_cast(bodyRot);
            vkCmdPushConstants(cmdBuffers_[frame], pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(glm::mat4), &bodyModel);
            VkDeviceSize vbOffset = 0;
            vkCmdBindVertexBuffers(cmdBuffers_[frame], 0, 1, &m.vertexBuffer, &vbOffset);
            vkCmdBindIndexBuffer(cmdBuffers_[frame], m.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmdBuffers_[frame], m.indexCount, 1, 0, 0, 0);
        }
    }

    // 实体趟：每个实体绑自己那张贴图的 descriptor，逐骨骼推世界矩阵后 draw
    if (!entityMeshes_.empty()) {
        std::vector<glm::mat4> boneWorld;
        for (const auto& [id, em] : entityMeshes_) {
            if (em.textureSet == VK_NULL_HANDLE) continue;    // 没贴图就不画
            const auto pit = entityPose_.find(id);
            if (pit == entityPose_.end()) continue;
            if (!pit->second.visible) continue;               // 第一人称隐藏自身

            vkCmdBindDescriptorSets(cmdBuffers_[frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout_, 0, 1, &em.textureSet, 0, nullptr);

            const glm::vec3 pos{static_cast<float>(pit->second.pos.x),
                                static_cast<float>(pit->second.pos.y),
                                static_cast<float>(pit->second.pos.z)};
            const glm::mat4 rootWorld =
                glm::translate(glm::mat4(1.0f), pos) *
                glm::rotate(glm::mat4(1.0f), glm::radians(pit->second.yawDeg), glm::vec3(0.0f, 1.0f, 0.0f));

            // 骨骼世界矩阵：父矩阵 × (平移到枢轴 → 局部旋转 → 平移回来)
            // 目前局部旋转恒为单位矩阵（未做动画），骨骼顶点位置与建模时一致
            boneWorld.assign(em.bones.size(), glm::mat4(1.0f));
            for (std::size_t k = 0; k < em.order.size(); ++k) {
                const int bi = em.order[k];
                if (bi < 0 || bi >= static_cast<int>(em.bones.size())) continue;
                const int par = em.parents[bi];
                const glm::mat4& parentW = (par >= 0) ? boneWorld[par] : rootWorld;
                const glm::vec3& piv = em.pivots[bi];
                const glm::mat4 local =
                    glm::translate(glm::mat4(1.0f), piv) *
                    glm::translate(glm::mat4(1.0f), -piv);
                boneWorld[bi] = parentW * local;
            }

            for (std::size_t bi = 0; bi < em.bones.size(); ++bi) {
                const EntityBoneGpu& g = em.bones[bi];
                if (g.indexCount == 0) continue;
                vkCmdPushConstants(cmdBuffers_[frame], pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(glm::mat4), &boneWorld[bi]);
                VkDeviceSize vbOffset = 0;
                vkCmdBindVertexBuffers(cmdBuffers_[frame], 0, 1, &g.vertexBuffer, &vbOffset);
                vkCmdBindIndexBuffer(cmdBuffers_[frame], g.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmdBuffers_[frame], g.indexCount, 1, 0, 0, 0);
            }
        }

        // 换回图集那套：后面的 Cutout 趟还在用方块贴图
        vkCmdBindDescriptorSets(cmdBuffers_[frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 0, 1, &descSet_, 0, nullptr);
    }

    // 第二趟：Cutout 桶（透明/镂空后画，双面 cull NONE）
    vkCmdBindPipeline(cmdBuffers_[frame], VK_PIPELINE_BIND_POINT_GRAPHICS, cutoutPipeline_);
    vkCmdPushConstants(cmdBuffers_[frame], pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(glm::mat4), &identityMatrix);
    for (const auto& [cp, m] : chunkMeshes_) {
        if (m.cutoutVertexCount == 0) continue;   // 无 Cutout 面跳过
        VkDeviceSize vbOffset = 0;
        vkCmdBindVertexBuffers(cmdBuffers_[frame], 0, 1, &m.cutoutVertexBuffer, &vbOffset);
        vkCmdBindIndexBuffer(cmdBuffers_[frame], m.cutoutIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmdBuffers_[frame], m.cutoutIndexCount, 1, 0, 0, 0);
    }

    // 第三趟：HUD 调试浮层（F3 切换）。屏幕空间、关深度、alpha 混合，
    // 复用上面已设好的整窗 viewport/scissor，不影响 Solid/Cutout 两趟。
    if (hudVisible_) {
        drawHud(cmdBuffers_[frame], frame);
    }

    vkCmdEndRenderPass(cmdBuffers_[frame]);
    if (vkEndCommandBuffer(cmdBuffers_[frame]) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "vkEndCommandBuffer 失败");
        return false;
    }

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &imageAvailable_[frame];
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffers_[frame];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &renderFinished_[imageIndex];
    r = vkQueueSubmit(graphicsQueue_, 1, &submitInfo, inFlightFences_[frame]);
    if (r != VK_SUCCESS) {
        std::ostringstream os;
        os << "vkQueueSubmit 失败 result=" << r;
        ENGINE_LOG_ERROR("Vulkan", os.str());
        return false;
    }

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished_[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex;
    r = vkQueuePresentKHR(graphicsQueue_, &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || framebufferResized_) {
        framebufferResized_ = false;
        recreateSwapchain();
        return false;
    }
    if (r != VK_SUCCESS) {
        std::ostringstream os;
        os << "vkQueuePresentKHR 失败 result=" << r;
        ENGINE_LOG_ERROR("Vulkan", os.str());
        return false;
    }
    ++framesPresented_;
    return true;
}

// GLFW 帧缓冲尺寸回调：窗口尺寸变化时置位重建标志（drawFrame 帧首检查）
void VulkanContext::framebufferResizeCallback(GLFWwindow* w, int, int) {
    auto* self = static_cast<VulkanContext*>(glfwGetWindowUserPointer(w));
    if (self) {
        self->framebufferResized_ = true;
        std::ostringstream os;
        os << "[回调] framebufferResized_ 已置位";
        ENGINE_LOG_INFO("Vulkan", os.str());
    }
}
// 释放描述符池/布局/UBO 缓冲
void VulkanContext::destroyUniformObjects() {
    if (descPool_) { vkDestroyDescriptorPool(device_, descPool_, nullptr); descPool_ = VK_NULL_HANDLE; }
    if (descLayout_) { vkDestroyDescriptorSetLayout(device_, descLayout_, nullptr); descLayout_ = VK_NULL_HANDLE; }
    if (uboBuffer_) { vkDestroyBuffer(device_, uboBuffer_, nullptr); uboBuffer_ = VK_NULL_HANDLE; }
    if (uboMemory_) { vkFreeMemory(device_, uboMemory_, nullptr); uboMemory_ = VK_NULL_HANDLE; }
}



// 释放深度图像/视图/显存
void VulkanContext::destroyDepthResources() {
    if (depthImageView_) { vkDestroyImageView(device_, depthImageView_, nullptr); depthImageView_ = VK_NULL_HANDLE; }
    if (depthImageMemory_) { vkFreeMemory(device_, depthImageMemory_, nullptr); depthImageMemory_ = VK_NULL_HANDLE; }
    if (depthImage_) { vkDestroyImage(device_, depthImage_, nullptr); depthImage_ = VK_NULL_HANDLE; }
}

// 逆创建序释放全部渲染对象
void VulkanContext::destroyRenderObjects() {
    renderObjectsReady_ = false;
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (inFlightFences_[i]) { vkDestroyFence(device_, inFlightFences_[i], nullptr); inFlightFences_[i] = VK_NULL_HANDLE; }
        if (imageAvailable_[i]) { vkDestroySemaphore(device_, imageAvailable_[i], nullptr); imageAvailable_[i] = VK_NULL_HANDLE; }
    }
    for (auto& sem : renderFinished_) {
        if (sem) { vkDestroySemaphore(device_, sem, nullptr); sem = VK_NULL_HANDLE; }
    }
    renderFinished_.clear();
    if (cmdBuffers_[0]) { vkFreeCommandBuffers(device_, cmdPool_, kMaxFramesInFlight, cmdBuffers_); }
    if (cmdPool_) { vkDestroyCommandPool(device_, cmdPool_, nullptr); cmdPool_ = VK_NULL_HANDLE; }
    for (auto fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    framebuffers_.clear();
    destroyDepthResources(); // 深度资源：帧缓冲之后、管线之前
    // 增量重建：销毁所有 chunk 网格（每 chunk 的 GPU 资源）
    for (auto& [cp, m] : chunkMeshes_) destroyChunkMesh(device_, m);
    chunkMeshes_.clear();
    // P14：延迟销毁队列里可能还挂着旧网格，关停时一并释放（此处已 vkDeviceWaitIdle）
    for (auto& bucket : graveyard_) {
        for (auto& m : bucket) destroyChunkMesh(device_, m);
        bucket.clear();
    }
    // 刚体网格：与 chunk 网格同结构，同样逐个销毁
    for (auto& [id, m] : bodyMeshes_) destroyChunkMesh(device_, m);
    bodyMeshes_.clear();
    bodyPos_.clear();
    bodyRot_.clear();
    // 实体：逐骨骼释放缓冲，再释放贴图三件套
    for (auto& [id, em] : entityMeshes_) {
        for (auto& g : em.bones) {
            if (g.vertexBuffer) vkDestroyBuffer(device_, g.vertexBuffer, nullptr);
            if (g.vertexMemory) vkFreeMemory(device_, g.vertexMemory, nullptr);
            if (g.indexBuffer)  vkDestroyBuffer(device_, g.indexBuffer, nullptr);
            if (g.indexMemory)  vkFreeMemory(device_, g.indexMemory, nullptr);
        }
    }
    entityMeshes_.clear();
    entityPose_.clear();
    // 实体贴图：按路径缓存的多份，逐份释放（descriptor set 随池统一回收，不单独销毁）
    for (auto& [path, tex] : entityTextures_) {
        if (tex.sampler) vkDestroySampler(device_, tex.sampler, nullptr);
        if (tex.view)    vkDestroyImageView(device_, tex.view, nullptr);
        if (tex.image)   vkDestroyImage(device_, tex.image, nullptr);
        if (tex.memory)  vkFreeMemory(device_, tex.memory, nullptr);
    }
    entityTextures_.clear();
    if (textureSampler_) { vkDestroySampler(device_, textureSampler_, nullptr); textureSampler_ = VK_NULL_HANDLE; }
    if (textureImageView_) { vkDestroyImageView(device_, textureImageView_, nullptr); textureImageView_ = VK_NULL_HANDLE; }
    if (textureImage_) { vkDestroyImage(device_, textureImage_, nullptr); textureImage_ = VK_NULL_HANDLE; }
    if (textureImageMemory_) { vkFreeMemory(device_, textureImageMemory_, nullptr); textureImageMemory_ = VK_NULL_HANDLE; }
    if (pipeline_) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (cutoutPipeline_) { vkDestroyPipeline(device_, cutoutPipeline_, nullptr); cutoutPipeline_ = VK_NULL_HANDLE; }
    if (pipelineLayout_) { vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr); pipelineLayout_ = VK_NULL_HANDLE; }
    destroyHudResources();   // HUD 管线引用 renderPass_，必须先于 renderPass 销毁
    if (renderPass_) { vkDestroyRenderPass(device_, renderPass_, nullptr); renderPass_ = VK_NULL_HANDLE; }
    destroyUniformObjects(); // UBO/描述符（在管线/命令之后释放）
}

// 轮询 GLFW 事件（主线程调用）
void VulkanContext::pollEvents() { glfwPollEvents(); } //检测对于窗口是否有事件发生

// 窗口是否请求关闭
bool VulkanContext::shouldClose() const {
    return window_ ? glfwWindowShouldClose(window_) != GLFW_FALSE : true;
}

// 完整关闭：销毁窗口/交换链/设备/实例
void VulkanContext::shutdown() {
    if (!window_ && !instance_ && !device_) return;

    if (device_) vkDeviceWaitIdle(device_);
    destroyRenderObjects();
    destroyColorResources();                                          // MSAA 颜色图三件套
    destroySwapchain();                                               // swapchain + 图像视图
    if (device_) { vkDestroyDevice(device_, nullptr); device_ = VK_NULL_HANDLE; }
    if (surface_) { vkDestroySurfaceKHR(instance_, surface_, nullptr); surface_ = VK_NULL_HANDLE; }
    if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
    if (window_) { glfwDestroyWindow(window_); window_ = nullptr; }
    if (glfwInitialized_) { glfwTerminate(); glfwInitialized_ = false; }
}

VulkanContext::~VulkanContext() { shutdown(); }

// ============================================================================
// HUD（游戏内调试浮层 / F3）
// 与 3D 双管线完全隔离：独立 pipelineLayout（只挂字体纹理）、关深度、alpha 混合，
// 画在同一个 renderPass 的最后一趟（Cutout 之后、EndRenderPass 之前）。
// ============================================================================

// 可见性切换（渲染线程调用）：打日志是为了让"F3 到底有没有送达/有没有被消费"可验证，
// 不然 HUD 不显示时分不清是没切换还是绘制出问题。
void VulkanContext::setHudVisible(bool visible) {
    hudVisible_ = visible;
    ENGINE_LOG_INFO("Vulkan", visible ? "[HUD] 显示（F3）" : "[HUD] 隐藏（F3）");
}

// 业务统计喂入：由渲染线程按 1Hz 调用。
// 为什么不用每帧：chunkCount()/blockCount() 内部要 lock_guard(blocksMutex_)，
// 每帧查等于让渲染帧率去和业务 tick 抢同一把锁。
// tps 由相邻两次采样的 tickCount 差分 / 墙钟差分估算（两次采样间隔就是刷新周期）。
void VulkanContext::setHudStats(long long chunkCount, long long blockCount,
                                long long tickCount, double simTime) {
    const auto now = std::chrono::steady_clock::now();
    if (hudInfo_.valid) {
        const double dtSec = std::chrono::duration<double>(now - tpsWindowStart_).count();
        if (dtSec > 0.05) {
            hudInfo_.tps = static_cast<double>(tickCount - tpsLastTicks_) / dtSec;
        }
    }
    tpsWindowStart_ = now;
    tpsLastTicks_ = tickCount;

    hudInfo_.chunkCount = chunkCount;
    hudInfo_.blockCount = blockCount;
    hudInfo_.tickCount = tickCount;
    hudInfo_.simTime = simTime;
    hudInfo_.valid = true;
}

// 字体图集：assets/textures/font/ascii.png（由 tools/gen_font_atlas.ps1 离线生成）
// 黑底白字，R 通道 = 覆盖率；第 0 格是纯白块（画背板用）。
bool VulkanContext::createFontTexture() {
    int w = 0, h = 0, ch = 0;
    stbi_uc* src = stbi_load("assets/textures/font/ascii.png", &w, &h, &ch, STBI_rgb_alpha);
    if (!src) {
        ENGINE_LOG_ERROR("Vulkan", "[HUD] 字体图集缺失：assets/textures/font/ascii.png"
                  "（运行 tools/gen_font_atlas.ps1 生成）");
        return false;
    }
    // 尺寸必须与代码常量一致，否则所有 UV 都会错位 → 宁可明确报错也不要画乱码
    if (w != hud::kAtlasW || h != hud::kAtlasH) {
        std::ostringstream os;
        os << "[HUD] 字体图集尺寸 " << w << "x" << h
           << " 与代码常量 " << hud::kAtlasW << "x" << hud::kAtlasH
           << " 不一致（重跑 tools/gen_font_atlas.ps1）";
        ENGINE_LOG_ERROR("Vulkan", os.str());
        stbi_image_free(src);
        return false;
    }

    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(w) * h * 4;
    if (!util::createImage(device_, physicalDevice_,
            static_cast<uint32_t>(w), static_cast<uint32_t>(h), VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            1, VK_SAMPLE_COUNT_1_BIT, fontImage_, fontImageMemory_)) {
        stbi_image_free(src);
        return false;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    if (!util::createBuffer(device_, physicalDevice_, imageSize,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging, stagingMem)) {
        stbi_image_free(src);
        return false;
    }
    void* data = nullptr;
    vkMapMemory(device_, stagingMem, 0, imageSize, 0, &data);
    std::memcpy(data, src, static_cast<std::size_t>(imageSize));
    vkUnmapMemory(device_, stagingMem);
    stbi_image_free(src);

    util::transitionImageLayout(device_, graphicsQueue_, cmdPool_, fontImage_,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    util::copyBufferToImage(device_, graphicsQueue_, cmdPool_, staging, fontImage_,
        static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    util::transitionImageLayout(device_, graphicsQueue_, cmdPool_, fontImage_,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);

    if (!util::createImageView(device_, fontImage_, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_ASPECT_COLOR_BIT, fontImageView_)) {
        ENGINE_LOG_ERROR("Vulkan", "[HUD] 字体纹理视图创建失败");
        return false;
    }

    // 采样器：NEAREST 必须（像素字体做线性插值会糊成一团），CLAMP 防止 UV 边缘渗到邻格
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(device_, &samplerInfo, nullptr, &fontSampler_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "[HUD] 字体采样器创建失败");
        return false;
    }

    std::ostringstream os;
    os << "[HUD] 字体图集已上传 " << w << "x" << h;
    ENGINE_LOG_INFO("Vulkan", os.str());
    return true;
}

// HUD 管线：2D 四边形，关深度、开 alpha 混合、cull NONE。
// 必须用 msaaSamples_ —— 同一个 renderPass 里所有管线采样数必须一致。
bool VulkanContext::createHudPipeline() {
    VkShaderModule vert = util::loadShaderModule(device_, "shaders/hud_vert.spv");
    if (vert == VK_NULL_HANDLE) return false;
    VkShaderModule frag = util::loadShaderModule(device_, "shaders/hud_frag.spv");
    if (frag == VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, vert, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(hud::HudVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // 与 shaders/hud.vert 的 location 0/1/2 一一对应
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;        // vec2 pos（像素）
    attrs[0].offset = static_cast<uint32_t>(offsetof(hud::HudVertex, pos));
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;        // vec2 uv
    attrs[1].offset = static_cast<uint32_t>(offsetof(hud::HudVertex, uv));
    attrs[2].location = 2;
    attrs[2].binding = 0;
    attrs[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;  // vec4 color
    attrs[2].offset = static_cast<uint32_t>(offsetof(hud::HudVertex, color));

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // 动态 viewport/scissor：直接复用 drawFrame 里已经设置好的整窗视口
    VkViewport viewport{};
    VkRect2D scissor{};
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dyn;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;   // 2D 四边形也参与 3D 面剔除没有意义
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    rasterizer.lineWidth = 1.0f;

    // 关深度：HUD 永远覆盖在 3D 画面上，不读也不写深度缓冲
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = msaaSamples_;

    // alpha 混合：背板半透明压暗背景，字形按覆盖率叠加
    VkPipelineColorBlendAttachmentState blendAttach{};
    blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttach.blendEnable = VK_TRUE;
    blendAttach.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttach.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttach.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttach.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttach.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttach;

    // push constant = vec2 屏幕尺寸（顶点阶段换算 NDC；resize 只改参数，不重建管线）
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(float) * 2;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &hudDescLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &hudPipelineLayout_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "[HUD] 管线布局创建失败");
        vkDestroyShaderModule(device_, vert, nullptr);
        vkDestroyShaderModule(device_, frag, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = hudPipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 0;

    const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                nullptr, &hudPipeline_);
    vkDestroyShaderModule(device_, vert, nullptr);
    vkDestroyShaderModule(device_, frag, nullptr);
    if (r != VK_SUCCESS) {
        std::ostringstream os;
        os << "[HUD] vkCreateGraphicsPipelines 失败 result=" << r;
        ENGINE_LOG_ERROR("Vulkan", os.str());
        return false;
    }
    ENGINE_LOG_INFO("Vulkan", "[HUD] 管线创建成功（depth off / alpha blend / MSAA 匹配主场景）");
    return true;
}

// HUD 全部 GPU 资源：字体纹理 → 描述符 → 管线 → 每帧顶点缓冲。
bool VulkanContext::createHudResources() {
    if (!createFontTexture()) return false;

    // 独立描述符布局/池：只放一张字体纹理，不和 3D 的 UBO 共享额度
    VkDescriptorSetLayoutBinding fontBinding{};
    fontBinding.binding = 0;
    fontBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    fontBinding.descriptorCount = 1;
    fontBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &fontBinding;
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &hudDescLayout_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "[HUD] 描述符布局创建失败");
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &hudDescPool_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "[HUD] 描述符池创建失败");
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = hudDescPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &hudDescLayout_;
    if (vkAllocateDescriptorSets(device_, &allocInfo, &hudDescSet_) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("Vulkan", "[HUD] 描述符集分配失败");
        return false;
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = fontImageView_;
    imageInfo.sampler = fontSampler_;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = hudDescSet_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    if (!createHudPipeline()) return false;

    // 顶点缓冲：每帧槽一份，host visible + coherent、持久映射（每帧只 memcpy 不 flush）
    const VkDeviceSize bufSize = static_cast<VkDeviceSize>(sizeof(hud::HudVertex)) * kHudVertexCapacity;
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (!util::createBuffer(device_, physicalDevice_, bufSize,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                hudVertexBuffer_[i], hudVertexMemory_[i])) {
            ENGINE_LOG_ERROR("Vulkan", "[HUD] 顶点缓冲创建失败");
            return false;
        }
        void* mapped = nullptr;
        if (vkMapMemory(device_, hudVertexMemory_[i], 0, bufSize, 0, &mapped) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("Vulkan", "[HUD] 顶点缓冲映射失败");
            return false;
        }
        hudVertexMapped_[i] = mapped;
    }
    ENGINE_LOG_INFO("Vulkan", "[HUD] 资源就绪（顶点容量 12288 = 2048 字符 × 6）");
    return true;
}

// 逆序释放（可重复调用：每个句柄销毁后立刻置空）
void VulkanContext::destroyHudResources() {
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (hudVertexMapped_[i] && hudVertexMemory_[i]) {
            vkUnmapMemory(device_, hudVertexMemory_[i]);
            hudVertexMapped_[i] = nullptr;
        }
        if (hudVertexBuffer_[i]) { vkDestroyBuffer(device_, hudVertexBuffer_[i], nullptr); hudVertexBuffer_[i] = VK_NULL_HANDLE; }
        if (hudVertexMemory_[i]) { vkFreeMemory(device_, hudVertexMemory_[i], nullptr); hudVertexMemory_[i] = VK_NULL_HANDLE; }
    }
    if (hudPipeline_) { vkDestroyPipeline(device_, hudPipeline_, nullptr); hudPipeline_ = VK_NULL_HANDLE; }
    if (hudPipelineLayout_) { vkDestroyPipelineLayout(device_, hudPipelineLayout_, nullptr); hudPipelineLayout_ = VK_NULL_HANDLE; }
    if (hudDescPool_) { vkDestroyDescriptorPool(device_, hudDescPool_, nullptr); hudDescPool_ = VK_NULL_HANDLE; hudDescSet_ = VK_NULL_HANDLE; }
    if (hudDescLayout_) { vkDestroyDescriptorSetLayout(device_, hudDescLayout_, nullptr); hudDescLayout_ = VK_NULL_HANDLE; }
    if (fontSampler_) { vkDestroySampler(device_, fontSampler_, nullptr); fontSampler_ = VK_NULL_HANDLE; }
    if (fontImageView_) { vkDestroyImageView(device_, fontImageView_, nullptr); fontImageView_ = VK_NULL_HANDLE; }
    if (fontImage_) { vkDestroyImage(device_, fontImage_, nullptr); fontImage_ = VK_NULL_HANDLE; }
    if (fontImageMemory_) { vkFreeMemory(device_, fontImageMemory_, nullptr); fontImageMemory_ = VK_NULL_HANDLE; }
    hudText_.clear();
}

// 组装本帧 HUD 文本 → 顶点流。
// 两档数据来源：相机（原子读，每帧）/ 业务统计（1Hz 由 Renderer 喂进 hudInfo_）。
std::size_t VulkanContext::buildHudVertices() {
    hudText_.clear();

    // ---- 相机（输入线程写、这里只读）----
    double cx = 0.0, cy = 0.0, cz = 0.0, yaw = 0.0, pitch = 0.0;
    if (cameraState_) {
        cx    = cameraState_->camX.load(std::memory_order_relaxed);
        cy    = cameraState_->camY.load(std::memory_order_relaxed);
        cz    = cameraState_->camZ.load(std::memory_order_relaxed);
        yaw   = cameraState_->yaw.load(std::memory_order_relaxed);
        pitch = cameraState_->pitch.load(std::memory_order_relaxed);
    }

    // ---- 朝向：yaw=0 朝 -Z（north），每 45° 一个方位，顺时针排列 ----
    constexpr double kTwoPi = 6.283185307179586;
    double normYaw = std::fmod(yaw, kTwoPi);
    if (normYaw < 0.0) normYaw += kTwoPi;
    // 8 方位：四项正方向之外的斜向也标出坐标符号，风格统一（否则 "southwest" 与 "south (+Z)"
    // 混排，最长行长度随转身跳动，背板会跟着忽宽忽窄）。
    // 顺序按 yaw 递增（-Z 北方为 0），索引 = round(yaw / 45°) % 8。
    static const char* kFacing[8] = {
        "north (-Z)", "northeast (+X,-Z)", "east (+X)", "southeast (+X,+Z)",
        "south (+Z)", "southwest (-X,+Z)", "west (-X)", "northwest (-X,-Z)"};
    const int facingIndex = static_cast<int>(normYaw / (kTwoPi / 8.0) + 0.5) % 8;

    // ---- 所在 chunk + chunk 内局部坐标 ----
    // 先 floor 再位运算：bx & 15 对负数同样落在 0..15（补码），bx >> 4 是算术右移（向 -∞ 取整），
    // 与 ChunkPos.hpp 里 chunkOf 的约定一致。
    const int bx = static_cast<int>(std::floor(cx));
    const int by = static_cast<int>(std::floor(cy));
    const int bz = static_cast<int>(std::floor(cz));
    const int chunkX = bx >> 4, localX = bx & 15;
    const int chunkY = by >> 4, localY = by & 15;
    const int chunkZ = bz >> 4, localZ = bz & 15;

    // ---- 已提交的网格统计（纯内存遍历，不碰 GPU）----
    int meshChunks = 0;
    long long triCount = 0;
    for (const auto& [cp, m] : chunkMeshes_) {
        ++meshChunks;
        triCount += static_cast<long long>(m.indexCount / 3);
        triCount += static_cast<long long>(m.cutoutIndexCount / 3);
    }

    // ---- 文本行（英文标签：字体图集只覆盖 ASCII 32..126）----
    std::vector<std::string> lines;
    lines.reserve(10);
    char buf[200];

    std::snprintf(buf, sizeof(buf),
        "[F3] DEBUG OVERLAY     FPS %5.1f  (%.2f ms)", fpsSmoothed_, frameMsSmoothed_);
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "XYZ      %.2f / %.2f / %.2f", cx, cy, cz);
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "YAW      %6.1f   PITCH %6.1f   FACING %s",
        yaw * 57.2957795130, pitch * 57.2957795130, kFacing[facingIndex]);
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "CHUNK    (%d, %d, %d)   LOCAL (%d, %d, %d)",
        chunkX, chunkY, chunkZ, localX, localY, localZ);
    lines.emplace_back(buf);

    if (hudInfo_.valid) {
        std::snprintf(buf, sizeof(buf), "CHUNKS   %lld   BLOCKS %lld",
            hudInfo_.chunkCount, hudInfo_.blockCount);
        lines.emplace_back(buf);
        std::snprintf(buf, sizeof(buf), "TICK     sim %.1f s   ticks %lld   tps %.1f",
            hudInfo_.simTime, hudInfo_.tickCount, hudInfo_.tps);
        lines.emplace_back(buf);
    } else {
        lines.emplace_back("CHUNKS   --   BLOCKS --");
        lines.emplace_back("TICK     --");
    }

    std::snprintf(buf, sizeof(buf), "MESH     chunks %d   triangles %lld",
        meshChunks, triCount);
    lines.emplace_back(buf);

    std::snprintf(buf, sizeof(buf), "WINDOW   %ux%u   MSAA %dx   frames %llu",
        swapchainExtent_.width, swapchainExtent_.height,
        static_cast<int>(msaaSamples_),   // 枚举值本身即采样数（1/2/4/8/16）
        static_cast<unsigned long long>(framesPresented_));
    lines.emplace_back(buf);

    // ---- 排版：先算背板宽度 → 画板 → 画字（顶点顺序即绘制顺序，板必须先于字）----
    float maxWidth = 0.0f;
    for (const auto& line : lines) {
        const float w = hudText_.textWidth(line);
        if (w > maxWidth) maxWidth = w;   // 不用 std::max：Windows.h 的 max 宏会吃掉它
    }
    constexpr float kPanelMargin = 8.0f;  // 背板到窗口边缘
    constexpr float kInsetX = 10.0f;      // 文字到背板左缘
    constexpr float kInsetY = 8.0f;       // 文字到背板上缘
    const float panelW0 = maxWidth + kInsetX * 2.0f;
    // 背板宽度加下限：FPS 99→100、坐标位数变化会让"最长行"左右跳，背板跟着缩会看起来像在抖。
    // 只在内容真的更宽时才扩展，同时不能超出窗口（小窗口时按屏幕宽度收）。
    constexpr float kPanelMinW = 640.0f;
    float panelW = panelW0;
    if (panelW < kPanelMinW) panelW = kPanelMinW;
    const float panelMaxW = static_cast<float>(swapchainExtent_.width) - kPanelMargin * 2.0f;
    if (panelW > panelMaxW) panelW = panelMaxW;
    const float panelH = static_cast<float>(lines.size()) * hud::HudTextBuilder::kLineHeight
                       + kInsetY * 2.0f;
    hudText_.addPanel(kPanelMargin, kPanelMargin, panelW, panelH,
                      glm::vec4(0.0f, 0.0f, 0.0f, 0.55f));

    const glm::vec4 kTextColor(0.92f, 0.95f, 1.0f, 1.0f);
    const glm::vec4 kTitleColor(0.55f, 0.95f, 0.72f, 1.0f);
    float penY = kPanelMargin + kInsetY;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        hudText_.addText(kPanelMargin + kInsetX, penY, lines[i],
                         i == 0 ? kTitleColor : kTextColor);
        penY += hud::HudTextBuilder::kLineHeight;
    }

    // 容量保护：正常约 1000 顶点；超容量说明文本异常，截断而不是越界写 GPU 缓冲
    const std::size_t count = hudText_.vertexCount();
    if (count > kHudVertexCapacity) {
        ENGINE_LOG_WARN("Vulkan", "[HUD] 顶点数超容量，已截断（检查 HUD 文本长度）");
        return kHudVertexCapacity;
    }
    return count;
}

// 录制 HUD 这一趟：更新本帧顶点 → 绑定独立管线/描述符 → push 屏幕尺寸 → 一次 draw。
void VulkanContext::drawHud(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (!hudPipeline_ || !hudVertexMapped_[frameIndex]) return;

    const std::size_t vcount = buildHudVertices();
    if (vcount == 0) return;

    // 首帧绘制确认：证明 HUD 真的进了命令缓冲（只打一次，不污染每帧日志）
    if (!hudDrawnOnce_) {
        hudDrawnOnce_ = true;
        std::ostringstream os;
        os << "[HUD] 首帧绘制 " << vcount << " 顶点";
        ENGINE_LOG_INFO("Vulkan", os.str());
    }

    // HOST_COHERENT 内存：memcpy 之后不需要 vkFlushMappedMemoryRanges
    std::memcpy(hudVertexMapped_[frameIndex], hudText_.vertices().data(),
                vcount * sizeof(hud::HudVertex));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipelineLayout_,
                            0, 1, &hudDescSet_, 0, nullptr);
    VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &hudVertexBuffer_[frameIndex], &vbOffset);

    const float screen[2] = {
        static_cast<float>(swapchainExtent_.width),
        static_cast<float>(swapchainExtent_.height)};
    vkCmdPushConstants(cmd, hudPipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(screen), screen);

    vkCmdDraw(cmd, static_cast<uint32_t>(vcount), 1, 0, 0);
}

} // namespace engine::render
