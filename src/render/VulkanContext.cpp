// VulkanContext：GLFW 窗口 + volk 动态加载 + 实例 + 设备枚举 + 逻辑设备。
// 学习向注释：每个对象创建前解释"为什么"——Vulkan 无隐藏状态，一切显式。
#include "render/VulkanContext.hpp"
#include "core/Log.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <shaderc/shaderc.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <sstream>
#include <vector>

namespace engine::render {

namespace {

constexpr const char* kRequiredInstanceExtensions[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_WIN32_SURFACE_EXTENSION_NAME, // surface 扩展提前启用：下一步 swapchain 直接可用
};
constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

// Vulkan 不保证驱动带全量扩展/图层——必须先枚举再启用，硬塞会创建失败。
bool hasInstanceLayer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const auto& l : layers)
        if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}

bool hasInstanceExtension(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
    for (const auto& e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

const char* deviceTypeName(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "独立显卡";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "集成显卡";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "虚拟 GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU 软渲染";
    default:                                     return "未知";
    }
}

// 找一个"同时支持图形 + 呈现"的队列族（绝大多数消费级 GPU 满足；
// 分离队列族方案留待后期多队列需求，见架构文档"物理后期多线程"）。
uint32_t findGraphicsPresentFamily(VkPhysicalDevice dev, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
    std::vector<VkQueueFamilyProperties> fams(count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, fams.data());
    for (uint32_t i = 0; i < count; ++i) {
        if (!(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &supported);
        if (supported == VK_TRUE) return i;
    }
    return UINT32_MAX;
}

// ---- 着色器：GLSL 源码 → shaderc 编译 → VkShaderModule ----
// 免 LunarG SDK 路线：不依赖 glslangValidator 命令行，运行时编译。
constexpr const char* kVertexShaderGLSL = R"(
#version 450
layout(location = 0) in vec3 inPos;    // 顶点缓冲提供：方块局部坐标
layout(location = 1) in vec3 inColor;  // 顶点缓冲提供：顶点颜色
layout(binding = 0) uniform UniformBufferObject {  // 每帧更新：投影×视图矩阵
    mat4 mvp;
} ubo;
layout(location = 0) out vec3 fragColor;
void main() {
    gl_Position = ubo.mvp * vec4(inPos, 1.0);
    fragColor = inColor;
}
)";

constexpr const char* kFragmentShaderGLSL = R"(
#version 450
layout(location = 0) in vec3 fragColor;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(fragColor, 1.0);
}
)";

// ---- 方块几何：24 顶点 + 36 索引（每面独立 4 顶点、独立颜色）----
// 体素 mesh 标准做法：面不共享顶点（相邻面颜色/法线不同），
// 与将来 chunk mesh 的生成逻辑一致（每个方块 24 顶点 36 索引）。
std::vector<Vertex> buildCubeVertices() {
    const glm::vec3 c[6] = {
        {1.00f, 0.55f, 0.20f}, // +X 橙（木面）
        {0.55f, 0.35f, 0.15f}, // -X 深棕
        {0.75f, 0.60f, 0.35f}, // +Y 浅棕（顶）
        {0.45f, 0.30f, 0.15f}, // -Y 深棕（底）
        {0.85f, 0.50f, 0.25f}, // +Z 中棕
        {0.35f, 0.22f, 0.12f}, // -Z 最深棕
    };
    const float h = 0.5f; // 半边长：方块中心在原点，尺寸 1×1×1
    // 每个面的 4 个角（逆时针，配合 CCW front face）
    const glm::vec3 faces[6][4] = {
        // +X
        {{ h,-h,-h},{ h,-h, h},{ h, h, h},{ h, h,-h}},
        // -X
        {{-h,-h, h},{-h,-h,-h},{-h, h,-h},{-h, h, h}},
        // +Y
        {{-h, h,-h},{ h, h,-h},{ h, h, h},{-h, h, h}},
        // -Y
        {{-h,-h, h},{ h,-h, h},{ h,-h,-h},{-h,-h,-h}},
        // +Z
        {{-h,-h, h},{-h, h, h},{ h, h, h},{ h,-h, h}},
        // -Z
        {{ h,-h,-h},{-h,-h,-h},{-h, h,-h},{ h, h,-h}},
    };
    std::vector<Vertex> verts;
    verts.reserve(24);
    for (int f = 0; f < 6; ++f)
        for (int i = 0; i < 4; ++i)
            verts.push_back(Vertex{faces[f][i], c[f]});
    return verts;
}

std::vector<uint32_t> buildCubeIndices() {
    // 每面 2 个三角形（4 顶点按 0,1,2 / 0,2,3 拆分）
    std::vector<uint32_t> idx;
    idx.reserve(36);
    for (uint32_t f = 0; f < 6; ++f) {
        const uint32_t b = f * 4;
        idx.push_back(b + 0); idx.push_back(b + 1); idx.push_back(b + 2);
        idx.push_back(b + 0); idx.push_back(b + 2); idx.push_back(b + 3);
    }
    return idx;
}

VkShaderModule compileShader(VkDevice dev, VkShaderStageFlagBits stage,
                            const char* src) {
    // shaderc：GLSL → SPIR-V 运行时编译（Vulkan 官方推荐 libshaderc）。
    shaderc::Compiler compiler;
    const shaderc_shader_kind kind =
        stage == VK_SHADER_STAGE_VERTEX_BIT ? shaderc_vertex_shader
                                            : shaderc_fragment_shader;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan,
                                 shaderc_env_version_vulkan_1_0);
    options.SetTargetSpirv(shaderc_spirv_version_1_0);
    const auto result = compiler.CompileGlslToSpv(src, kind, "inline", options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        std::ostringstream os;
        os << "[Vulkan] 着色器编译失败: " << result.GetErrorMessage();
        LOG_ERROR(os.str());
        return VK_NULL_HANDLE;
    }
    // 拷 SPIR-V 字节 → VkShaderModule（管线创建后即可销毁，GPU 已复制）。
    std::vector<uint32_t> spirv(result.cbegin(), result.cend());
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = spirv.size() * sizeof(uint32_t);
    ci.pCode = spirv.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &mod) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreateShaderModule 失败");
        return VK_NULL_HANDLE;
    }
    return mod;
}

} // namespace

// ---- GLFW 输入回调（static 成员定义；经 user pointer 转发到 this）----
void VulkanContext::mouseButtonCb(GLFWwindow* w, int button, int action, int) {
    auto* self = static_cast<VulkanContext*>(glfwGetWindowUserPointer(w));
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;
    self->mouseDown_ = (action == GLFW_PRESS);
    if (self->mouseDown_) glfwGetCursorPos(w, &self->lastMouseX_, &self->lastMouseY_);
}

void VulkanContext::cursorPosCb(GLFWwindow* w, double x, double y) {
    auto* self = static_cast<VulkanContext*>(glfwGetWindowUserPointer(w));
    if (!self->mouseDown_) return;
    const double dx = x - self->lastMouseX_;
    const double dy = y - self->lastMouseY_;
    self->lastMouseX_ = x;
    self->lastMouseY_ = y;
    constexpr double kSens = 0.005; // 拖动灵敏度（弧度/像素）
    self->camYaw_   += static_cast<float>(dx * kSens);
    self->camPitch_ -= static_cast<float>(dy * kSens);
    self->camPitch_ = std::clamp(self->camPitch_, -1.45f, 1.45f); // ±83°，防翻跟头
}

VulkanContext::~VulkanContext() { shutdown(); }

bool VulkanContext::init(int width, int height, const char* title) {
    if (glfwInit() != GLFW_TRUE) {
        LOG_ERROR("[Vulkan] glfwInit 失败");
        return false;
    }
    glfwInitialized_ = true;

    // 纯 Vulkan 窗口：明确告诉 GLFW 不要建 OpenGL 上下文（省资源、省状态）。
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!window_) {
        LOG_ERROR("[Vulkan] glfwCreateWindow 失败");
        return false;
    }
    // 注册输入回调（user pointer 转发到 this；渲染线程 drawFrame 读状态）。
    glfwSetWindowUserPointer(window_, this);
    glfwSetMouseButtonCallback(window_, mouseButtonCb);
    glfwSetCursorPosCallback(window_, cursorPosCb);
    {
        std::ostringstream os;
        os << "[Vulkan] GLFW 窗口已创建 " << width << "x" << height;
        LOG_INFO(os.str());
    }

    // volk：运行时加载显卡驱动自带的 vulkan-1.dll（免装 LunarG SDK）。
    if (volkInitialize() != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] volkInitialize 失败（找不到 vulkan-1.dll？）");
        return false;
    }
    LOG_INFO("[Vulkan] volk 已加载 vulkan-1.dll（动态加载）");

    if (!createInstance()) return false;
    volkLoadInstance(instance_); // 实例创建后加载实例级函数指针（vkCreateDevice 等）
    if (!createSurface()) return false;
    if (!pickDevice()) return false;
    if (!createDevice()) return false;
    volkLoadDevice(device_);     // 设备创建后加载设备级函数指针（vkCreateSwapchainKHR 等）
    if (!createSwapchain()) return false;
    if (!createRenderPass()) return false;
    if (!createUniformObjects()) return false;
    if (!createPipeline()) return false;
    if (!createFramebuffers()) return false;
    if (!createMeshBuffers()) return false;
    if (!createCommandObjects()) return false;
    if (!createSyncObjects()) return false;
    renderObjectsReady_ = true;
    return true;
}

bool VulkanContext::createInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "SandboxEngine";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "sandbox-engine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0; // 最低要求 1.0；实际驱动通常 1.2/1.3

    // 实例扩展：surface 扩展提前启用（下一步 swapchain 建 surface 直接可用）。
    std::vector<const char*> enabledExts;
    for (const char* ext : kRequiredInstanceExtensions)
        if (hasInstanceExtension(ext)) enabledExts.push_back(ext);
        else {
            std::ostringstream os;
            os << "[Vulkan] 实例扩展不可用（跳过）: " << ext;
            LOG_WARN(os.str());
        }

    // 验证层：开发期开——Vulkan 会逐条检查 API 用法并打印精确错误，
    // 是新手最大的救命稻草；没有就跳过（发布环境/精简驱动正常）。
    std::vector<const char*> enabledLayers;
    if (hasInstanceLayer(kValidationLayer)) enabledLayers.push_back(kValidationLayer);
    else LOG_WARN("[Vulkan] 验证层不可用，跳过（发布环境正常）");

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &appInfo;
    ci.enabledExtensionCount = static_cast<uint32_t>(enabledExts.size());
    ci.ppEnabledExtensionNames = enabledExts.data();
    ci.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size());
    ci.ppEnabledLayerNames = enabledLayers.data();

    const VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
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

bool VulkanContext::createSurface() {
    // surface = 窗口与 GPU 的"连接点"（swapchain 必须挂在 surface 上）。
    // GLFW 内部自己动态解析 vkCreateWin32SurfaceKHR（不依赖编译期符号，volk 兼容）。
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

bool VulkanContext::pickDevice() {
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance_, &count, nullptr) != VK_SUCCESS || count == 0) {
        LOG_ERROR("[Vulkan] 未找到物理设备（无显卡？）");
        return false;
    }
    devices_.resize(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices_.data());

    for (std::size_t i = 0; i < devices_.size(); ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices_[i], &p);
        std::ostringstream os;
        os << "[Vulkan] 设备[" << i << "] " << p.deviceName
           << " | " << deviceTypeName(p.deviceType)
           << " | API " << VK_VERSION_MAJOR(p.apiVersion) << "."
           << VK_VERSION_MINOR(p.apiVersion) << "." << VK_VERSION_PATCH(p.apiVersion);
        LOG_INFO(os.str());
    }

    // 选块：优先"独立显卡 + 有 graphics/present 队列族"；
    // 没有离散卡则选第一块支持 surface 呈现的（核显也能跑）。
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    for (VkPhysicalDevice d : devices_) {
        if (findGraphicsPresentFamily(d, surface_) == UINT32_MAX) continue;
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

bool VulkanContext::createDevice() {
    // 队列族 = GPU 的"工人班组"（graphics/compute/transfer），
    // 类比我们引擎的业务/物理/输入线程——先枚举找 graphics 班组的编号。
    graphicsQueueFamily_ = findGraphicsPresentFamily(physicalDevice_, surface_);
    if (graphicsQueueFamily_ == UINT32_MAX) {
        LOG_ERROR("[Vulkan] 无 graphics+present 队列族");
        return false;
    }
    {
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &count, nullptr);
        std::vector<VkQueueFamilyProperties> fams(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &count, fams.data());
        std::ostringstream os;
        os << "[Vulkan] 选中队列族 #" << graphicsQueueFamily_
           << "（graphics+present，容量 " << fams[graphicsQueueFamily_].queueCount << "）";
        LOG_INFO(os.str());
    }

    // 设备扩展：swapchain 是绘制上屏必需（VK_KHR_swapchain）。
    const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = graphicsQueueFamily_;
    qci.queueCount = 1;          // 本步只要 1 条图形队列
    qci.pQueuePriorities = &priority;

    VkPhysicalDeviceFeatures features{}; // 本步不启用任何特性
    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = deviceExtensions;
    ci.pEnabledFeatures = &features;

    const VkResult r = vkCreateDevice(physicalDevice_, &ci, nullptr, &device_);
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

bool VulkanContext::createSwapchain() {
    // 交换链 = "画一帧、显示一帧"的循环通道：GPU 画到后备图像，完成后再呈现。
    // ① surface 能力：图像数量范围 / 当前尺寸。
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps);

    // ② surface 格式：优先标准 RGBA8 + sRGB（颜色空间），没有就取第一个。
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &fmtCount, fmts.data());
    VkSurfaceFormatKHR chosen{};
    bool found = false;
    for (const auto& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            found = true;
            break;
        }
    }
    if (!found) chosen = fmts[0];
    swapchainFormat_ = chosen.format;

    // ③ 呈现模式：MAILBOX（低延迟，画面可跳帧）优先，退化 FIFO（垂直同步最稳）。
    uint32_t pmCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &pmCount, nullptr);
    std::vector<VkPresentModeKHR> modes(pmCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &pmCount, modes.data());
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto m : modes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) { presentMode = m; break; }
    }

    // ④ 图像数量：min+1（多一帧缓冲减少等待），不超过 max 上限。
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    // ⑤ 尺寸：surface 报当前尺寸则用它（窗口尺寸变化时靠重建交换链，本步先不处理）。
    if (caps.currentExtent.width != UINT32_MAX) {
        swapchainExtent_ = caps.currentExtent;
    } else {
        int w = 0, h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        swapchainExtent_ = { static_cast<uint32_t>(w), static_cast<uint32_t>(h) };
    }

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surface_;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = swapchainExtent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; // 作为颜色附件被渲染
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;     // 单队列族（我们的同族方案）
    ci.preTransform = caps.currentTransform;              // 不额外旋转
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;    // 不透明窗口
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;                                 // 被遮挡部分不绘制（性能）
    ci.oldSwapchain = VK_NULL_HANDLE;                     // 重建时用它（本步无）

    const VkResult r = vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_);
    if (r != VK_SUCCESS) {
        std::ostringstream os;
        os << "[Vulkan] vkCreateSwapchainKHR 失败 result=" << r;
        LOG_ERROR(os.str());
        return false;
    }

    // ⑥ 取回实际图像 + 每个图像建视图（下一步 RenderPass/Framebuffer 用）。
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

void VulkanContext::destroySwapchain() {
    for (auto v : swapchainImageViews_) vkDestroyImageView(device_, v, nullptr);
    swapchainImageViews_.clear();
    swapchainImages_.clear();
    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool VulkanContext::createRenderPass() {
    // RenderPass = 告诉 GPU"这帧怎么画"：附件格式/读写规则 + 子pass。
    // 一个颜色附件（swapchain 图像）：每帧开头 CLEAR（清屏），结尾 STORE（保留到呈现）。
    VkAttachmentDescription color{};
    color.format = swapchainFormat_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;          // 进来不知道什么布局
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;      // 出去要能呈现

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    // 隐式布局转换（UNDEFINED→COLOR_ATTACHMENT→PRESENT_SRC）的同步依赖，
    // 经典模板：保证清屏写入在呈现前完成。
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 1;
    ci.pAttachments = &color;
    ci.subpassCount = 1;
    ci.pSubpasses = &subpass;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;

    if (vkCreateRenderPass(device_, &ci, nullptr, &renderPass_) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreateRenderPass 失败");
        return false;
    }
    LOG_INFO("[Vulkan] RenderPass 创建成功（1 颜色附件：CLEAR→画→STORE 呈现）");
    return true;
}

bool VulkanContext::createUniformObjects() {
    // 描述符 = 着色器访问 GPU 资源的"门牌"：
    //   layout 声明门牌长什么样（binding 0 = uniform buffer，顶点阶段可见）
    //   pool 分配门牌内存，set 就是具体一块门牌，最后把 UBO 缓冲挂上去。
    // UBO 缓冲：HOST_VISIBLE（CPU 每帧重写 MVP），与 mesh 缓冲同策略。
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
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &props);
    uint32_t memIdx = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memIdx = i;
            break;
        }
    }
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
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 1;
    li.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &descLayout_) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreateDescriptorSetLayout 失败");
        return false;
    }

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device_, &pi, nullptr, &descPool_) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreateDescriptorPool 失败");
        return false;
    }

    VkDescriptorSetAllocateInfo dsa{};
    dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsa.descriptorPool = descPool_;
    dsa.descriptorSetCount = 1;
    dsa.pSetLayouts = &descLayout_;
    if (vkAllocateDescriptorSets(device_, &dsa, &descSet_) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkAllocateDescriptorSets 失败");
        return false;
    }

    VkDescriptorBufferInfo binfo{uboBuffer_, 0, sizeof(UniformBufferObject)};
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = descSet_;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w.pBufferInfo = &binfo;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);

    LOG_INFO("[Vulkan] MVP uniform 就绪（descriptor set + UBO，相机可操作）");
    return true;
}

bool VulkanContext::createPipeline() {
    // 图形管线 = 顶点着色器 + 片元着色器 + 顶点布局 + 光栅化/混合状态，
    // 一个完整"怎么画"的固化配方。Vulkan 最重的样板，但一次建好反复用。
    VkShaderModule vert = compileShader(device_, VK_SHADER_STAGE_VERTEX_BIT, kVertexShaderGLSL);
    if (vert == VK_NULL_HANDLE) return false;
    VkShaderModule frag = compileShader(device_, VK_SHADER_STAGE_FRAGMENT_BIT, kFragmentShaderGLSL);
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

    // 顶点输入：Vertex{pos: vec3, color: vec3}——一个绑定两个属性（偏移按结构体布局）。
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = static_cast<uint32_t>(offsetof(Vertex, pos));
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset = static_cast<uint32_t>(offsetof(Vertex, color));
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};
    viewport.x = 0.0f; viewport.y = 0.0f;
    viewport.width  = static_cast<float>(swapchainExtent_.width);
    viewport.height = static_cast<float>(swapchainExtent_.height);
    viewport.minDepth = 0.0f; viewport.maxDepth = 1.0f;
    VkRect2D scissor{{0, 0}, swapchainExtent_};
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;      // 先不剔除，看得到双面
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT; // 先不开 MSAA

    // 颜色混合：一个附件，直接覆盖（无混合）。
    VkPipelineColorBlendAttachmentState blendAttach{};
    blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttach.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttach;

    // 布局：绑定 MVP 描述符（descriptor set 0，uniform buffer）。
    VkPipelineLayoutCreateInfo layoutCi{};
    layoutCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCi.setLayoutCount = 1;
    layoutCi.pSetLayouts = &descLayout_;
    if (vkCreatePipelineLayout(device_, &layoutCi, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreatePipelineLayout 失败");
        vkDestroyShaderModule(device_, vert, nullptr);
        vkDestroyShaderModule(device_, frag, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vertexInput;
    ci.pInputAssemblyState = &inputAssembly;
    ci.pViewportState = &viewportState;
    ci.pRasterizationState = &rasterizer;
    ci.pMultisampleState = &multisample;
    ci.pColorBlendState = &colorBlend;
    ci.layout = pipelineLayout_;
    ci.renderPass = renderPass_;
    ci.subpass = 0;

    const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline_);
    // 着色器模块在管线创建后即可销毁（GPU 已复制进管线）。
    vkDestroyShaderModule(device_, vert, nullptr);
    vkDestroyShaderModule(device_, frag, nullptr);
    if (r != VK_SUCCESS) {
        std::ostringstream os;
        os << "[Vulkan] vkCreateGraphicsPipelines 失败 result=" << r;
        LOG_ERROR(os.str());
        return false;
    }
    LOG_INFO("[Vulkan] 图形管线创建成功（顶点着色器/片元着色器已编译）");
    return true;
}

bool VulkanContext::createFramebuffers() {
    // 帧缓冲 = "附件具体实例"：renderpass 规则 + 具体 swapchain 图像视图。
    // 每张交换链图像一个帧缓冲（画到哪张就用哪张的）。
    framebuffers_.clear();
    framebuffers_.reserve(swapchainImageViews_.size());
    for (VkImageView view : swapchainImageViews_) {
        VkFramebufferCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass = renderPass_;
        ci.attachmentCount = 1;
        ci.pAttachments = &view;
        ci.width = swapchainExtent_.width;
        ci.height = swapchainExtent_.height;
        ci.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (vkCreateFramebuffer(device_, &ci, nullptr, &fb) != VK_SUCCESS) {
            LOG_ERROR("[Vulkan] vkCreateFramebuffer 失败");
            return false;
        }
        framebuffers_.push_back(fb);
    }
    {
        std::ostringstream os;
        os << "[Vulkan] 帧缓冲创建成功（" << framebuffers_.size() << " 个，每交换链图像一个）";
        LOG_INFO(os.str());
    }
    return true;
}

bool VulkanContext::createMeshBuffers() {
    // 顶点/索引缓冲：把方块几何从 CPU 交给 GPU。
    // 本阶段用 HOST_VISIBLE|HOST_COHERENT 单缓冲（CPU 直接写，显卡读）；
    // 性能阶段换 device local + staging 上传（体素量大了再优化，见学习路线 ③）。
    const auto vertices = buildCubeVertices();
    const auto indices  = buildCubeIndices();
    vertexCount_ = static_cast<uint32_t>(vertices.size());
    indexCount_  = static_cast<uint32_t>(indices.size());

    // 申请内存类型：host 可见 + coherent（不需要显式 flush）
    auto allocBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                           VkBuffer& buf, VkDeviceMemory& mem) -> bool {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = size;
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &bi, nullptr, &buf) != VK_SUCCESS) {
            LOG_ERROR("[Vulkan] vkCreateBuffer 失败");
            return false;
        }
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, buf, &req);
        VkPhysicalDeviceMemoryProperties props;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &props);
        uint32_t memIdx = UINT32_MAX;
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                memIdx = i;
                break;
            }
        }
        if (memIdx == UINT32_MAX) {
            LOG_ERROR("[Vulkan] 无 host 可见内存类型");
            return false;
        }
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memIdx;
        if (vkAllocateMemory(device_, &ai, nullptr, &mem) != VK_SUCCESS) {
            LOG_ERROR("[Vulkan] vkAllocateMemory 失败");
            return false;
        }
        vkBindBufferMemory(device_, buf, mem, 0);
        return true;
    };
    auto upload = [&](VkDeviceMemory mem, VkDeviceSize size, const void* src) {
        void* dst = nullptr;
        vkMapMemory(device_, mem, 0, size, 0, &dst);
        std::memcpy(dst, src, static_cast<std::size_t>(size));
        vkUnmapMemory(device_, mem);
    };

    if (!allocBuffer(sizeof(Vertex) * vertexCount_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     vertexBuffer_, vertexBufferMemory_)) return false;
    upload(vertexBufferMemory_, sizeof(Vertex) * vertexCount_, vertices.data());

    if (!allocBuffer(sizeof(uint32_t) * indexCount_, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     indexBuffer_, indexBufferMemory_)) return false;
    upload(indexBufferMemory_, sizeof(uint32_t) * indexCount_, indices.data());

    {
        std::ostringstream os;
        os << "[Vulkan] 方块 mesh 就绪：顶点=" << vertexCount_ << " 索引=" << indexCount_
           << "（6 面独立着色，单位立方体）";
        LOG_INFO(os.str());
    }
    return true;
}

bool VulkanContext::createCommandObjects() {
    // 命令池：分配命令缓冲的"工厂"（绑定 graphics 队列族）。
    VkCommandPoolCreateInfo poolCi{};
    poolCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // 允许每帧 reset
    poolCi.queueFamilyIndex = graphicsQueueFamily_;
    if (vkCreateCommandPool(device_, &poolCi, nullptr, &cmdPool_) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkCreateCommandPool 失败");
        return false;
    }
    // 本步单条命令缓冲（单帧 in-flight）；多帧流水线优化留到性能阶段。
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = cmdPool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &alloc, &cmdBuffer_) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkAllocateCommandBuffers 失败");
        return false;
    }
    LOG_INFO("[Vulkan] 命令池 + 命令缓冲就绪（每帧录制绘制指令）");
    return true;
}

bool VulkanContext::createSyncObjects() {
    // 同步三件套：
    //   imageAvailable_：交换链有可用图像了（acquire 完成信号）
    //   renderFinished_：本帧画完了（提交完成信号，present 等它）
    //   inFlightFence_：GPU 执行完本帧提交（CPU 等它，防提前录下一帧）
    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT; // 初始已 signaled：第一帧 wait 直接过

    if (vkCreateSemaphore(device_, &si, nullptr, &imageAvailable_) != VK_SUCCESS ||
        vkCreateSemaphore(device_, &si, nullptr, &renderFinished_) != VK_SUCCESS ||
        vkCreateFence(device_, &fi, nullptr, &inFlightFence_) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] 同步对象创建失败");
        return false;
    }
    LOG_INFO("[Vulkan] 同步三件套就绪（acquire信号/完成信号/执行栅栏）");
    return true;
}

void VulkanContext::updateCamera(double dt) {
    // 键盘平移（WASD + 空格/Shift），速度 ~2.5 单位/秒（dt≈16ms 时每帧 ~0.04）。
    const float speed = 2.5f * static_cast<float>(dt);
    // 前进方向：只取 yaw 的水平分量（保持在地平面移动），pitch 只影响视角。
    // 约定 yaw=0 时面向 -Z（Vulkan/OpenGL 惯例，方块在 -Z 方向可见）。
    const glm::vec3 forward(std::sin(camYaw_), 0.0f, -std::cos(camYaw_));
    const glm::vec3 right(std::cos(camYaw_), 0.0f, std::sin(camYaw_)); // 右手系右向量
    if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS) camPos_ += forward * speed;
    if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS) camPos_ -= forward * speed;
    if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS) camPos_ += right * speed;
    if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS) camPos_ -= right * speed;
    if (glfwGetKey(window_, GLFW_KEY_SPACE) == GLFW_PRESS) camPos_.y += speed;
    if (glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) camPos_.y -= speed;
    if (glfwGetKey(window_, GLFW_KEY_R) == GLFW_PRESS) { // 重置视角
        camPos_ = {0.0f, 0.0f, 3.5f};
        camYaw_ = 0.0f;
        camPitch_ = 0.0f;
    }

    // MVP：投影 × 视图（模型 = 单位矩阵，方块在原点）。
    const float aspect = static_cast<float>(swapchainExtent_.width) /
                         static_cast<float>(swapchainExtent_.height);
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 100.0f);
    proj[1][1] *= -1.0f; // Vulkan 的 NDC Y 轴向下；glm 按 OpenGL 设计，需翻转

    const glm::vec3 lookDir(
        std::sin(camYaw_) * std::cos(camPitch_),
        std::sin(camPitch_),
        -std::cos(camYaw_) * std::cos(camPitch_)); // -Z 前方，方块默认在视野内
    const glm::mat4 view = glm::lookAt(camPos_, camPos_ + lookDir, glm::vec3(0.0f, 1.0f, 0.0f));

    UniformBufferObject ubo{};
    ubo.mvp = proj * view;
    void* data = nullptr;
    vkMapMemory(device_, uboMemory_, 0, sizeof(ubo), 0, &data);
    std::memcpy(data, &ubo, sizeof(ubo));
    vkUnmapMemory(device_, uboMemory_);
}

bool VulkanContext::drawFrame() {
    if (!renderObjectsReady_) return false;
    updateCamera(0.016); // 渲染线程 ~60Hz，固定步长（FIFO vsync 下接近真实 dt）

    // ① CPU 等 GPU：上一帧提交执行完（防止一帧没画完就录下一帧）。
    vkWaitForFences(device_, 1, &inFlightFence_, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &inFlightFence_);

    // ② 从交换链拿一张"可画的"图像（阻塞等 vsync 也行）。
    uint32_t imageIndex = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                       imageAvailable_, VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return true; // 窗口变了：跳过本帧（重建留后期）
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        std::ostringstream os;
        os << "[Vulkan] vkAcquireNextImageKHR 失败 result=" << r;
        LOG_ERROR(os.str());
        return false;
    }

    // ③ 录制绘制指令到命令缓冲。
    vkResetCommandBuffer(cmdBuffer_, 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(cmdBuffer_, &beginInfo) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkBeginCommandBuffer 失败");
        return false;
    }
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = renderPass_;
    rp.framebuffer = framebuffers_[imageIndex];
    rp.renderArea = {{0, 0}, swapchainExtent_};
    VkClearValue clear{};
    clear.color = {{0.02f, 0.02f, 0.05f, 1.0f}}; // 深蓝黑清屏
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmdBuffer_, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmdBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmdBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &descSet_, 0, nullptr);
    VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cmdBuffer_, 0, 1, &vertexBuffer_, &vbOffset);
    vkCmdBindIndexBuffer(cmdBuffer_, indexBuffer_, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmdBuffer_, indexCount_, 1, 0, 0, 0); // 方块：36 索引
    vkCmdEndRenderPass(cmdBuffer_);
    if (vkEndCommandBuffer(cmdBuffer_) != VK_SUCCESS) {
        LOG_ERROR("[Vulkan] vkEndCommandBuffer 失败");
        return false;
    }

    // ④ 提交：等 acquire 信号 → 执行绘制 → 发完成信号。
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAvailable_;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmdBuffer_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished_;
    r = vkQueueSubmit(graphicsQueue_, 1, &submit, inFlightFence_);
    if (r != VK_SUCCESS) {
        std::ostringstream os;
        os << "[Vulkan] vkQueueSubmit 失败 result=" << r;
        LOG_ERROR(os.str());
        return false;
    }

    // ⑤ 呈现：等完成信号 → 把画完的图像交还给窗口。
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished_;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex;
    r = vkQueuePresentKHR(graphicsQueue_, &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) return true;
    if (r != VK_SUCCESS) {
        std::ostringstream os;
        os << "[Vulkan] vkQueuePresentKHR 失败 result=" << r;
        LOG_ERROR(os.str());
        return false;
    }
    ++framesPresented_;
    return true;
}

void VulkanContext::destroyUniformObjects() {
    if (descPool_) { vkDestroyDescriptorPool(device_, descPool_, nullptr); descPool_ = VK_NULL_HANDLE; }
    if (descLayout_) { vkDestroyDescriptorSetLayout(device_, descLayout_, nullptr); descLayout_ = VK_NULL_HANDLE; }
    if (uboBuffer_) { vkDestroyBuffer(device_, uboBuffer_, nullptr); uboBuffer_ = VK_NULL_HANDLE; }
    if (uboMemory_) { vkFreeMemory(device_, uboMemory_, nullptr); uboMemory_ = VK_NULL_HANDLE; }
}

void VulkanContext::destroyRenderObjects() {
    renderObjectsReady_ = false;
    if (inFlightFence_) { vkDestroyFence(device_, inFlightFence_, nullptr); inFlightFence_ = VK_NULL_HANDLE; }
    if (imageAvailable_) { vkDestroySemaphore(device_, imageAvailable_, nullptr); imageAvailable_ = VK_NULL_HANDLE; }
    if (renderFinished_) { vkDestroySemaphore(device_, renderFinished_, nullptr); renderFinished_ = VK_NULL_HANDLE; }
    if (cmdBuffer_) { vkFreeCommandBuffers(device_, cmdPool_, 1, &cmdBuffer_); cmdBuffer_ = VK_NULL_HANDLE; }
    if (cmdPool_) { vkDestroyCommandPool(device_, cmdPool_, nullptr); cmdPool_ = VK_NULL_HANDLE; }
    for (auto fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    framebuffers_.clear();
    if (indexBuffer_) { vkDestroyBuffer(device_, indexBuffer_, nullptr); indexBuffer_ = VK_NULL_HANDLE; }
    if (indexBufferMemory_) { vkFreeMemory(device_, indexBufferMemory_, nullptr); indexBufferMemory_ = VK_NULL_HANDLE; }
    if (vertexBuffer_) { vkDestroyBuffer(device_, vertexBuffer_, nullptr); vertexBuffer_ = VK_NULL_HANDLE; }
    if (vertexBufferMemory_) { vkFreeMemory(device_, vertexBufferMemory_, nullptr); vertexBufferMemory_ = VK_NULL_HANDLE; }
    if (pipeline_) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (pipelineLayout_) { vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr); pipelineLayout_ = VK_NULL_HANDLE; }
    if (renderPass_) { vkDestroyRenderPass(device_, renderPass_, nullptr); renderPass_ = VK_NULL_HANDLE; }
    destroyUniformObjects(); // UBO/描述符（在管线/命令之后释放）
}

void VulkanContext::pollEvents() { glfwPollEvents(); }

bool VulkanContext::shouldClose() const {
    return window_ ? glfwWindowShouldClose(window_) != GLFW_FALSE : true;
}

void VulkanContext::shutdown() {
    // 释放顺序与创建顺序相反（Vulkan 惯例：后建先毁）。
    // 先等 GPU 干完手里的活，再销毁任何对象（避免用已销毁对象）。
    if (device_) vkDeviceWaitIdle(device_);
    destroyRenderObjects();                                        // 管线/帧缓冲/同步/命令
    destroySwapchain();                                            // swapchain + 图像视图
    if (device_) { vkDestroyDevice(device_, nullptr); device_ = VK_NULL_HANDLE; }
    if (surface_) { vkDestroySurfaceKHR(instance_, surface_, nullptr); surface_ = VK_NULL_HANDLE; }
    if (instance_) { vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE; }
    if (window_) { glfwDestroyWindow(window_); window_ = nullptr; }
    if (glfwInitialized_) { glfwTerminate(); glfwInitialized_ = false; }
}

} // namespace engine::render
