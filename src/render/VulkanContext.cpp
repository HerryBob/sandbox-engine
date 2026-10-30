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
#include "shared/block/Block.hpp"   
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
//相邻面剔除相关
#include "render/optimization/meshing/FaceCulling.hpp"
#include <unordered_set>
#include "registry/BlockRegistry.hpp"
#include "render/assets/ModelRegistry.hpp"
namespace engine::render {

// 初始化渲染上下文：窗口→实例→设备→交换链→管线→每帧对象，任一失败返回 false
bool VulkanContext::init(int width, int height, const char* title) {
    if (glfwInit() != GLFW_TRUE) {
        LOG_ERROR("[Vulkan] glfw 初始化失败");
        return false;
    }
    glfwInitialized_ = true;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!window_) {
        LOG_ERROR("[Vulkan] glfwCreateWindow 失败");
        return false;
    }
    glfwSetWindowUserPointer(window_, this);
    // 键盘/鼠标回调由 input::InputSampler::attach 注册（真实输入归输入线程）。
    glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback); // 窗口尺寸变化 → 置位重建标志
    {
        std::ostringstream os;
        os << "[Vulkan] GLFW 窗口已创建 " << width << "x" << height;
        LOG_INFO(os.str());
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
        LOG_ERROR("[Vulkan] 无支持的深度格式");
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
    if (!createPipeline()) return false;          // ← 移到这：引用 descLayout_
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
            os << "[Vulkan] 实例扩展不可用（跳过）: " << ext;
            LOG_WARN(os.str());
        }

    std::vector<const char*> enabledLayers;
    if (util::hasInstanceLayer(util::kValidationLayer)) enabledLayers.push_back(util::kValidationLayer);
    else LOG_WARN("[Vulkan] 验证层不可用，跳过（发布环境正常）");

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
        os << "[Vulkan] vkCreateInstance 失败 result=" << r;
        LOG_ERROR(os.str());
        return false;
    }
    {
        std::ostringstream os;
        os << "[Vulkan] 实例创建成功（扩展 " << enabledExts.size()
           << " 验证层 " << enabledLayers.size() << "）";
        LOG_INFO(os.str());
    }
    return true;
}

// 创建窗口表面（GLFW 窗口 ↔ 程序的桥梁）
bool VulkanContext::createSurface() {
    const VkResult r = glfwCreateWindowSurface(instance_, window_, nullptr, &surface_);
    if (r != VK_SUCCESS) {
        std::ostringstream os;
        os << "[Vulkan] glfwCreateWindowSurface 失败 result=" << r;
        LOG_ERROR(os.str());
        return false;
    }
    LOG_INFO("[Vulkan] surface 创建成功（窗口↔GPU 连接点）");
    return true;
}

// 枚举物理设备，选第一个满足 图形+呈现+扩展 的显卡
bool VulkanContext::pickDevice() {
    devices_ = util::enumeratePhysicalDevices(instance_);
    if (devices_.empty()) {
        LOG_ERROR("[Vulkan] 未找到物理设备（无显卡？）");
        return false;
    }

    for (std::size_t i = 0; i < devices_.size(); ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices_[i], &p);
        std::ostringstream os;
        os << "[Vulkan] 设备[" << i << "] " << p.deviceName
           << " | " << util::deviceTypeName(p.deviceType)
           << " | API " << VK_VERSION_MAJOR(p.apiVersion) << "."
           << VK_VERSION_MINOR(p.apiVersion) << "." << VK_VERSION_PATCH(p.apiVersion);
        LOG_INFO(os.str());
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
        LOG_ERROR("[Vulkan] 无支持 surface 呈现的设备");
        return false;
    }
    physicalDevice_ = chosen;
    return true;
}

// 用选中队列族创建逻辑设备，取图形/呈现队列
bool VulkanContext::createDevice() {
    graphicsQueueFamily_ = util::findGraphicsPresentFamily(physicalDevice_, surface_);
    if (graphicsQueueFamily_ == UINT32_MAX) {
        LOG_ERROR("[Vulkan] 无 graphics+present 队列族");
        return false;
    }
    {
        std::ostringstream os;
        os << "[Vulkan] 选中队列族 #" << graphicsQueueFamily_
           << "（graphics+present，容量 " << util::queueFamilyQueueCount(physicalDevice_, graphicsQueueFamily_) << "）";
        LOG_INFO(os.str());
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
        os << "[Vulkan] vkCreateDevice 失败 result=" << r;
        LOG_ERROR(os.str());
        return false;
    }
    vkGetDeviceQueue(device_, graphicsQueueFamily_, 0, &graphicsQueue_);
    LOG_INFO("[Vulkan] 逻辑设备创建成功（图形队列就绪）");
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
        os << "[Vulkan] vkCreateSwapchainKHR 失败 result=" << r;
        LOG_ERROR(os.str());
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
            LOG_ERROR("[Vulkan] vkCreateImageView 失败");
            return false;
        }
        swapchainImageViews_.push_back(view);
    }

    {
        std::ostringstream os;
        os << "[Vulkan] 交换链创建成功：图像=" << swapchainImages_.size()
           << " 格式=" << swapchainFormat_
           << " 尺寸=" << swapchainExtent_.width << "x" << swapchainExtent_.height
           << " 呈现=" << (presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX(低延迟)" : "FIFO(vsync)");
        LOG_INFO(os.str());
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
        os << "[Vulkan] [重建] 触发 recreateSwapchain（新尺寸 "
           << w << "x" << h << "）";
        LOG_INFO(os.str());
    }
    if (w == 0 || h == 0) {
        // 窗口最小化/拖成 0 尺寸：extent 无效，无法重建 → 直接返回，等下一帧再试
        // （绝不能 while 阻塞等事件：渲染线程会卡死，主线程 join 也跟着卡死）
        std::ostringstream os;
        os << "[Vulkan] [重建] 尺寸为 0，跳过本帧（等窗口恢复）";
        LOG_INFO(os.str());
        return false;
    }
    vkDeviceWaitIdle(device_); // 必须！确保所有队列上的旧资源引用已执行完
    for (auto fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr); // 帧缓冲引用旧视图，必须销毁
    framebuffers_.clear();
    destroySwapchain();        // 交换链 + 图像视图
    destroyColorResources();   // MSAA 颜色图（尺寸绑定）
    destroyDepthResources();   // 深度图（尺寸绑定）
    if (!createSwapchain()) { LOG_ERROR("[Vulkan] 重建失败：swapchain");     return false; }
    if (!createColorResources()) { LOG_ERROR("[Vulkan] 重建失败：MSAA 颜色图"); return false; }
    if (!createDepthResources()) { LOG_ERROR("[Vulkan] 重建失败：深度图");      return false; }
    if (!createFramebuffers()) { LOG_ERROR("[Vulkan] 重建失败：帧缓冲");      return false; }
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
        LOG_ERROR("[Vulkan] vkCreateRenderPass 失败");
        return false;
    }
    LOG_INFO("[Vulkan] RenderPass 创建成功（颜色 CLEAR→画→STORE 呈现 + 深度 CLEAR→画→丢弃）");
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
        LOG_ERROR("[Vulkan] 深度图像创建失败");
        return false;
    }

    // 视图：固定样板走通用工具（2D、深度 aspect、全 mip 全层）
    if (!util::createImageView(device_, depthImage_, depthFormat_,
            VK_IMAGE_ASPECT_DEPTH_BIT, depthImageView_)) {
        LOG_ERROR("[Vulkan] 深度图像视图创建失败");
        return false;
    }
    {
        std::ostringstream os;
        os << "[Vulkan] 深度缓冲就绪（格式 " << util::depthFormatName(depthFormat_)
           << " " << swapchainExtent_.width << "x" << swapchainExtent_.height << "）";
        LOG_INFO(os.str());
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
        LOG_ERROR("[Vulkan] MSAA 颜色视图创建失败");
        return false;
    }
    {
        std::ostringstream os;
        os << "[Vulkan] MSAA 颜色附件就绪（"
            << swapchainExtent_.width << "x" << swapchainExtent_.height
            << " @ " << msaaSamples_ << "x 采样）";
        LOG_INFO(os.str());
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
    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreatePipelineLayout 失败");
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
        os << "[Vulkan] vkCreateGraphicsPipelines 失败 result=" << r;
        LOG_ERROR(os.str());
        return false;
    }
    pipeline_ = out[0];
    cutoutPipeline_ = out[1];
    LOG_INFO("[Vulkan] 图形管线创建成功（Solid + Cutout 双管线）");
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
            LOG_ERROR("[Vulkan] vkCreateFramebuffer 失败");
            return false;
        }
        framebuffers_.push_back(fb);
    }
    {
        std::ostringstream os;
        os << "[Vulkan] 帧缓冲创建成功（" << framebuffers_.size()
           << " 个，每交换链图像一个，含深度附件）";
        LOG_INFO(os.str());
    }
    return true;
}

// 建立通用索引缓冲
bool VulkanContext::rebuildMeshFromBlocks(const std::vector<engine::shared::Block>& blocks) {
    // 全量重建 = 增量重建的特例：推导出全部 chunk → 逐个重建（首帧/世界重建用）。
    std::unordered_set<engine::shared::ChunkPos> allChunks;
    for (const auto& b : blocks) {
        if (b.type == engine::shared::BlockType::Air || b.contraptionId != -1) continue;
        allChunks.insert(engine::shared::chunkOf(b.pos)); // 现成工具：BlockPos → ChunkPos
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
        
    vkDeviceWaitIdle(device_); // GPU 安全点：等所有在飞命令完成，才能安全销毁旧 buffer
    //待重建集合
    std::unordered_set<engine::shared::ChunkPos> rebuildSet = dirtyChunks;
    //solid集合
    std::unordered_set<engine::shared::BlockPos> solid;
    solid.reserve(blocks.size());
    for (const auto& b : blocks) {
        if (b.contraptionId == -1) {
            //查询注册表: 注册过且 opaque = true 
            const auto* info = engine::registry::g_blockRegistry.find(
                static_cast<std::uint8_t>(b.type));
            if (info && info->opaque) {
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
        for (const auto& b : blocks) {
            if (b.type == engine::shared::BlockType::Air || b.contraptionId != -1) continue;
            if (b.pos.x >= bx && b.pos.x < bx + 16 &&
                b.pos.y >= by && b.pos.y < by + 16 &&
                b.pos.z >= bz && b.pos.z < bz + 16)
                chunkBlocks.push_back(b);
        }
        //生成chunk的顶点和索引（分桶：Solid 默认组 / Cutout 透明组）
        std::vector<Vertex> vertices;           // Solid 组顶点
        std::vector<uint32_t> indices;          // Solid 组索引
        std::vector<Vertex> cutoutVerts;        // Cutout 组顶点（玻璃等）
        std::vector<uint32_t> cutoutIndices;    // Cutout 组索引
        uint32_t vertexOffset = 0;
        uint32_t cutoutVertexOffset = 0;
        for (const auto& block : chunkBlocks) {
            const glm::vec3 center{block.pos.x + 0.5f, block.pos.y + 0.5f, block.pos.z + 0.5f};
            // 查注册表：renderType 决定进哪个桶（认属性不认方块身份）
            const auto* info = engine::registry::g_blockRegistry.find(
                static_cast<std::uint8_t>(block.type));
            const bool isCutout = info && info->renderType == engine::registry::RenderType::Cutout;
            auto& dstVerts   = isCutout ? cutoutVerts : vertices;
            auto& dstIndices = isCutout ? cutoutIndices : indices;
            auto& dstOffset  = isCutout ? cutoutVertexOffset : vertexOffset;
            // 玻璃整体淡蓝；solid 不传 tint 用默认逐面色
            const std::uint8_t faceMask = optimization::meshing::computeFaceMask(block.pos, solid);
            // 纹理 → UV：注册表纹理名 → 图集格 → UV 矩形（未注册 → default 兜底格）
            glm::vec4 uvRect = engine::assets::TextureAtlas::fallbackUV();
            if (info && info->texture) {
                engine::assets::AtlasSlot slot;
                if (textureAtlas_.slotOf(info->texture, slot))
                    uvRect = engine::assets::TextureAtlas::slotUV(slot);
            }
            // 顶点生成：优先数据驱动模型（模型名注册且加载成功 → buildModelVertices；
            // 无 model / 模型缺失 → 立方体兜底，仍用注册表纹理）
            std::vector<Vertex> vs;
            if (info && info->model) {
                if (const auto* md = engine::assets::g_modelRegistry.find(std::string(info->model)))
                    vs = mesh::buildModelVertices(center, *md, faceMask, textureAtlas_);
            }
            if (vs.empty())
                vs = mesh::buildCubeVertices(center, faceMask, uvRect);
            dstVerts.insert(dstVerts.end(), vs.begin(), vs.end());
            const uint32_t faceCount = static_cast<uint32_t>(vs.size() / 4);
            for (uint32_t f = 0; f < faceCount; ++f) {
                const uint32_t b0 = dstOffset + f * 4;
                dstIndices.push_back(b0 + 0); dstIndices.push_back(b0 + 1); dstIndices.push_back(b0 + 2);
                dstIndices.push_back(b0 + 0); dstIndices.push_back(b0 + 2); dstIndices.push_back(b0 + 3);
            }
            dstOffset += static_cast<uint32_t>(vs.size());
        }
        //无chunk 删除旧网格, 继续下一个
        auto it =  chunkMeshes_.find(cp);
        if (vertices.empty() && cutoutVerts.empty()) {
            if (it != chunkMeshes_.end()) {
                destroyChunkMesh(device_, it->second);
                chunkMeshes_.erase(it);
            }
            continue;
        }
        //有网格
        if (it != chunkMeshes_.end()) {
            destroyChunkMesh(device_, it->second);
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

        //重建日志（面数 = Solid + Cutout）
        {
            std::ostringstream os;
            os << "[Vulkan] 重建 chunk=(" << cp.x << "," << cp.y << "," << cp.z
                << ") 面=" << (m.indexCount + m.cutoutIndexCount) / 6
                << " 顶点=" << (m.vertexCount + m.cutoutVertexCount);
            LOG_INFO(os.str());
        }
    }
    return true;
}


// 创建命令池 + 每帧命令缓冲（2 条轮转）
bool VulkanContext::createCommandObjects() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // 允许每帧 reset
    poolInfo.queueFamilyIndex = graphicsQueueFamily_;
    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &cmdPool_) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreateCommandPool 失败");
        return false;
    }
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = cmdPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kMaxFramesInFlight;
    if (vkAllocateCommandBuffers(device_, &allocInfo, cmdBuffers_) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkAllocateCommandBuffers 失败");
        return false;
    }
    LOG_INFO("[Vulkan] 命令池 + 命令缓冲就绪（每帧槽一条，2 条轮转）");
    return true;
}

// 纹理图像：staging 搬运（构建图集 → 上传显存 → 转 SHADER_READ_ONLY）
// 图集 = 一张 128² 大纹理装所有方块小纹理（每格 16²，空槽棋盘格）
bool VulkanContext::createTextureImage() {
    if (!textureAtlas_.buildFromRegistry()) {   // 遍历注册表 → 拼图集（CPU 内存 128² RGBA）
        LOG_ERROR("[Vulkan] 图集构建失败");
        return false;
    }
    const auto& pixels = textureAtlas_.rgba();
    const std::uint32_t w = engine::assets::kAtlasSize;   // 128
    const std::uint32_t h = engine::assets::kAtlasSize;
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
        LOG_ERROR("[Vulkan] 纹理视图创建失败");
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
        LOG_ERROR("[Vulkan] 纹理采样器创建失败");
        return false;
    }
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
        LOG_ERROR("[Vulkan] UBO vkCreateBuffer 失败");
        return false;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, uboBuffer_, &req);
    uint32_t memIdx = util::findMemoryType(physicalDevice_, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memIdx == UINT32_MAX) {
        LOG_ERROR("[Vulkan] UBO 无 host 可见内存类型");
        return false;
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memIdx;
    if (vkAllocateMemory(device_, &ai, nullptr, &uboMemory_) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] UBO vkAllocateMemory 失败");
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
        LOG_ERROR("[Vulkan] vkCreateDescriptorSetLayout 失败");
        return false;
    }

    VkDescriptorPoolSize poolSizes[2] = {
    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},   // ← 新增
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descPool_) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreateDescriptorPool 失败");
        return false;
    }

    VkDescriptorSetAllocateInfo setAllocInfo{};
    setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAllocInfo.descriptorPool = descPool_;
    setAllocInfo.descriptorSetCount = 1;
    setAllocInfo.pSetLayouts = &descLayout_;
    if (vkAllocateDescriptorSets(device_, &setAllocInfo, &descSet_) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkAllocateDescriptorSets 失败");
        return false;
    }

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

    LOG_INFO("[Vulkan] MVP uniform 就绪（descriptor set + UBO，相机可操作）");
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
            LOG_ERROR("[Vulkan] 同步对象创建失败");
            return false;
        }
    }
    renderFinished_.resize(swapchainImages_.size(), VK_NULL_HANDLE);
    for (auto& sem : renderFinished_) {
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &sem) != VK_SUCCESS) {
            LOG_ERROR("[Vulkan] renderFinished 信号量创建失败");
            return false;
        }
    }
    LOG_INFO("[Vulkan] 同步对象就绪（acquire×2 轮转 + renderFinished×图像数 + 栅栏×2）");
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
    ubo.model = glm::mat4(1.0f);  // 方块在原点：单位矩阵
    ubo.view  = view;
    ubo.proj  = proj;
    void* data = nullptr;
    vkMapMemory(device_, uboMemory_, 0, sizeof(ubo), 0, &data);
    std::memcpy(data, &ubo, sizeof(ubo));
    vkUnmapMemory(device_, uboMemory_);
}

// 每帧绘制：acquire → 录制 → submit → present
bool VulkanContext::drawFrame() {
    if (!renderObjectsReady_) return false;
    updateViewProj(0.006); // 帧间隔 6.06ms（165Hz 目标；FIFO vsync 拉齐到显示器刷新）

    const uint32_t frame =
        static_cast<uint32_t>(framesPresented_) % kMaxFramesInFlight;
    vkWaitForFences(device_, 1, &inFlightFences_[frame], VK_TRUE, UINT64_MAX);
    // 注意：vkResetFences 必须在 acquire 成功之后！
    // acquire 失败(OUT_OF_DATE)时 fence 保持 signaled → 下一帧 wait 直接通过
    // 若先 reset，失败帧没有 submit 触发 fence → 下一帧 wait 无限阻塞 → 渲染线程卡死

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
        os << "[Vulkan] vkAcquireNextImageKHR 失败 result=" << r;
        LOG_ERROR(os.str());
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
        LOG_ERROR("[Vulkan] vkBeginCommandBuffer 失败");
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

    // 第一趟：Solid 桶（不透明先画，写深度建立遮挡基准）
    vkCmdBindPipeline(cmdBuffers_[frame], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    for (const auto& [cp, m] : chunkMeshes_) {
        if (m.vertexCount == 0) continue;   // 无 Solid 面跳过
        VkDeviceSize vbOffset = 0;
        vkCmdBindVertexBuffers(cmdBuffers_[frame], 0, 1, &m.vertexBuffer, &vbOffset);
        vkCmdBindIndexBuffer(cmdBuffers_[frame], m.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmdBuffers_[frame], m.indexCount, 1, 0, 0, 0);
    }

    // 第二趟：Cutout 桶（透明/镂空后画，双面 cull NONE）
    vkCmdBindPipeline(cmdBuffers_[frame], VK_PIPELINE_BIND_POINT_GRAPHICS, cutoutPipeline_);
    for (const auto& [cp, m] : chunkMeshes_) {
        if (m.cutoutVertexCount == 0) continue;   // 无 Cutout 面跳过
        VkDeviceSize vbOffset = 0;
        vkCmdBindVertexBuffers(cmdBuffers_[frame], 0, 1, &m.cutoutVertexBuffer, &vbOffset);
        vkCmdBindIndexBuffer(cmdBuffers_[frame], m.cutoutIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmdBuffers_[frame], m.cutoutIndexCount, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(cmdBuffers_[frame]);
    if (vkEndCommandBuffer(cmdBuffers_[frame]) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkEndCommandBuffer 失败");
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
        os << "[Vulkan] vkQueueSubmit 失败 result=" << r;
        LOG_ERROR(os.str());
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
        os << "[Vulkan] vkQueuePresentKHR 失败 result=" << r;
        LOG_ERROR(os.str());
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
        os << "[Vulkan] [回调] framebufferResized_ 已置位";
        LOG_INFO(os.str());
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
    if (textureSampler_) { vkDestroySampler(device_, textureSampler_, nullptr); textureSampler_ = VK_NULL_HANDLE; }
    if (textureImageView_) { vkDestroyImageView(device_, textureImageView_, nullptr); textureImageView_ = VK_NULL_HANDLE; }
    if (textureImage_) { vkDestroyImage(device_, textureImage_, nullptr); textureImage_ = VK_NULL_HANDLE; }
    if (textureImageMemory_) { vkFreeMemory(device_, textureImageMemory_, nullptr); textureImageMemory_ = VK_NULL_HANDLE; }
    if (pipeline_) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (cutoutPipeline_) { vkDestroyPipeline(device_, cutoutPipeline_, nullptr); cutoutPipeline_ = VK_NULL_HANDLE; }
    if (pipelineLayout_) { vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr); pipelineLayout_ = VK_NULL_HANDLE; }
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

} // namespace engine::render
