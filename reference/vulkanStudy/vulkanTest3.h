#pragma once
// 练习队列与族
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>

#include <iostream>
#include <stdexcept>
#include <functional>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <windows.h> //解决控制台输出中文乱码

#include <set> //自动去重
#include <fstream>
//校验层常开
#ifdef NDEBUG
const bool enableValidationLayers = false;
#else
const bool enableValidationLayers = true;
#endif // NDEBUG

const std::vector<const char*> validationLayers = { "VK_LAYER_KHRONOS_validation"};

//窗口大小
const uint32_t WIDTH = 800;
const uint32_t HEIGHT = 600;


// 呈现模式枚举 → 名字
const char* presentModeName(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR:      return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR:         return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    default: return "UNKNOWN";
    }
}

// 常见格式枚举 → 名字
const char* formatName(VkFormat format) {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_SRGB:  return "R8G8B8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    default: return "OTHER";
    }
}


struct QueueFamilyIndices {

    int graphicsFamily = -1;
    int presentFamily = -1;

    bool isComplete() {
        return graphicsFamily >= 0 && presentFamily >= 0;
    }


};

class VulkanTest3 {
protected:
    //校验层////////////////////////////////////////////////////////////////////
        //校验层的收件箱句柄
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;    //问题通知的收件地址
    //体检(检查loader是否存在需要的层(校验层))
    bool CheckValidationLayerSupport() {
        uint32_t layerCount;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
        for (const char* layerName : validationLayers) {
            bool layerFound = false;
            for (const auto& layerProperties : availableLayers) {
                if (strcmp(layerName, layerProperties.layerName) == 0) {
                    layerFound = true;
                    break;
                }
            }
            if (!layerFound) {
                return false;
            }
        }
        return true;
    }
    //拓展合并(将GLFW和校验层的扩展合并)
    std::vector<const char*> getRequiredExtensions() {
        //首先是GLFW要求的扩展
        uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions;
        glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);
        if (enableValidationLayers) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        return extensions;
    }

    //收件箱收件规则
    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, //严重级别 'error''warning''info''verbose'
        VkDebugUtilsMessageTypeFlagsEXT messageType,            //消息类型 'general''validation''performance'
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,//消息内容
        void* pUserData) {                                      //用户数据
        std::cerr << "校验层: " << pCallbackData->pMessage << std::endl;
        return VK_FALSE;
    }

    //运行加载 
    VkResult CreateDebugUtilsMessengerEXT(VkInstance instance,
        const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDebugUtilsMessengerEXT* pDebugMessenger) {
        auto func = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");

        if (func != nullptr) {
            return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
        }
        else {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }
    //创建收件箱
    void setupDebugCallback() {
        if (!enableValidationLayers) {
            return;
        }
        //开始填表
        VkDebugUtilsMessengerCreateInfoEXT createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        //收件严重级别: 只要是警告和错误都收
        createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        //收件类型
        createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

        createInfo.pfnUserCallback = debugCallback;   // ③ 投递地址（第 6 步函数）
        createInfo.pUserData = nullptr;               // ④ 用户数据

        if (CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &debugMessenger) != VK_SUCCESS) {
            throw std::runtime_error("创建收件箱失败!");//正式创建
        }
    }
    //运行卸载
    void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator) {
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (func != nullptr) {
            func(instance, debugMessenger, pAllocator);
        }
    }

    //校验层////////////////////////////////////////////////////////////////////

        //初始化 Vulkan 
    VkInstance instance = VK_NULL_HANDLE; //实例句柄
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE; //显卡句柄
    VkQueue graphicsQueue = VK_NULL_HANDLE; //队列句柄
    QueueFamilyIndices queueFamilyIndices; //存找到的族，供 createLogicalDevice 用

    //交换链内容开始
        //surface句柄
    VkSurfaceKHR surface;
    //交换链句柄
    VkSwapchainKHR swapChain = VK_NULL_HANDLE;
    //将交换链的几个参数提高为类成员
    uint32_t imageCount = 0;
    VkFormat swapChainImageFormat;
    VkExtent2D swapChainExtent;
    //开始为每张画布创建图像视图
    std::vector<VkImage> swapChainImages; //交换链图像句柄数组
    std::vector<VkImageView> swapChainImageViews;
    //渲染管线层
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    //渲染通道
    VkRenderPass renderPass = VK_NULL_HANDLE;
    //图形管线
    VkPipeline graphicsPipeline = VK_NULL_HANDLE;
    //帧缓冲区
    std::vector<VkFramebuffer> swapChainFramebuffers;
    //命令缓冲
    VkCommandPool commandPool;
    //VkCommandBuffer commandBuffer;
    std::vector<VkCommandBuffer> commandBuffers;  // 新：3 本轮换（每帧一本）
    //渲染循环
    VkQueue presentQueue = VK_NULL_HANDLE;      // 呈现队列（从 findQueueFamilies 取）
    std::vector<VkSemaphore> imageAvailableSemaphores;  // "画布可用"信号
    std::vector<VkSemaphore> renderFinishedSemaphores;  // "画完了"信号
    std::vector<VkFence> inFlightFences;        // "上一帧干完了"栅栏
    std::vector<VkFence> imagesInFlight;        // 每张画布被哪一帧占用（防撞）
    uint32_t currentFrame = 0;                  // 当前轮换到第几帧
    //修饰符插槽清单
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;   // 插槽清单

    //深度缓冲
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthImageMemory = VK_NULL_HANDLE;
    VkImageView depthImageView = VK_NULL_HANDLE;
    VkFormat depthFormat;

    //mass颜色图
    VkImage colorBuffer;            // msaa 颜色图（绘制目标）
    VkDeviceMemory colorBufferMemory;
    VkImageView colorBufferView;    // 它的视图

    //抗锯齿
    VkSampleCountFlagBits msaaSamples;   // 抗锯齿采样次数（2/4/8...）
    

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) &&
                (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        throw std::runtime_error("找不到符合要求的内存类型!");
    }
    //深度缓冲二维图 
    void createImage(uint32_t width, uint32_t height, VkFormat format,
        VkImageUsageFlags usage,
        uint32_t mipLevels, //多级纹理
        VkSampleCountFlagBits sampls,//新增采样次数
        VkImage& image, VkDeviceMemory& imageMemory) {
        //表1 
        VkImageCreateInfo imageInfo = {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { width, height, 1 };
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = sampls;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        vkCreateImage(device, &imageInfo, nullptr, &image);
        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(device, image, &memReq);

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;                              // 用驱动算的大小
        allocInfo.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);                            // 深度图放设备本地显存
        if (vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
            throw std::runtime_error("分配显存失败!");
        }

        vkBindImageMemory(device, image, imageMemory, 0);

    }
    //询问显卡支持那种深度格式
    VkFormat findDepthFormat() {
        std::vector<VkFormat> formats = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT , VK_FORMAT_D16_UNORM_S8_UINT };
        for (VkFormat format : formats) {
            VkFormatProperties props;
            vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);
            if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                return format;
            }
        }
        throw std::runtime_error("找不到支持的深度格式!");
    }
    //询问显卡支持的MASS采样次数
    VkSampleCountFlagBits getMaxUsableSampleCount() {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physicalDevice, &props);
        VkSampleCountFlags counts = 
            props.limits.framebufferColorSampleCounts &           // 颜色附件能采样几次
            props.limits.framebufferDepthSampleCounts;            // 深度附件能采样几次（要两者都支持）
        // 从高到低试探：想要 8x，显卡不给就用 4x、2x、最后 1x（无抗锯齿）
        if (counts & VK_SAMPLE_COUNT_64_BIT) return VK_SAMPLE_COUNT_64_BIT;
        if (counts & VK_SAMPLE_COUNT_32_BIT) return VK_SAMPLE_COUNT_32_BIT;
        if (counts & VK_SAMPLE_COUNT_16_BIT) return VK_SAMPLE_COUNT_16_BIT;
        if (counts & VK_SAMPLE_COUNT_8_BIT)  return VK_SAMPLE_COUNT_8_BIT;
        if (counts & VK_SAMPLE_COUNT_4_BIT)  return VK_SAMPLE_COUNT_4_BIT;
        if (counts & VK_SAMPLE_COUNT_2_BIT)  return VK_SAMPLE_COUNT_2_BIT;
        return VK_SAMPLE_COUNT_1_BIT;                             // 最低保底：无抗锯齿
    }
    //组装深度函数
    void createDepthResources() {
            depthFormat = findDepthFormat();
            createImage(WIDTH, HEIGHT, depthFormat,                // ② 造图
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                1, msaaSamples, depthImage, depthImageMemory);
                // 创建图像视图的代码
                VkImageViewCreateInfo createInfo = {};
                createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                createInfo.image = depthImage;
                createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                createInfo.format = depthFormat;        // ← 依赖①赋值
                createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
                createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
                createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
                createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
                createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                createInfo.subresourceRange.baseMipLevel = 0;
                createInfo.subresourceRange.levelCount = 1;
                createInfo.subresourceRange.baseArrayLayer = 0;
                createInfo.subresourceRange.layerCount = 1;

                VkResult result = vkCreateImageView(device, &createInfo, nullptr, &depthImageView);
                if (result != VK_SUCCESS) {
                    throw std::runtime_error("创建深度视图失败!");
                }
    }
    //多采样颜色附件（抗锯齿的绘制目标）
    void createColorResources() {
        //造图
        createImage(swapChainExtent.width, swapChainExtent.height,
            swapChainImageFormat,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
            1,
            msaaSamples,
            colorBuffer, colorBufferMemory);
        //创建视图
        VkImageViewCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = colorBuffer;                    // ← 图换成 colorBuffer
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = swapChainImageFormat;          // ← 格式用交换链格式
        createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;       // ← 视图只要 1 级（它没 mipmap）
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &createInfo, nullptr, &colorBufferView) != VK_SUCCESS) {
            throw std::runtime_error("创建颜色附件视图失败!");
        }
    }

    //将二进制文件读内存
    static std::vector<char> readFile(const std::string& filename) {
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            throw std::runtime_error("无法打开文件: " + filename);
        }

        size_t fileSize = (size_t)file.tellg();
        std::vector<char> buffer(fileSize);
        file.seekg(0);
        file.read(buffer.data(), fileSize);

        file.close();
        return buffer;
    }
    //把字节包成 VkShaderModule
    VkShaderModule createShaderModule(const std::vector<char>& code) {
        VkShaderModuleCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule shaderModule;
        VkResult result = vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule);
        if (result != VK_SUCCESS)
        {
            throw std::runtime_error("创建着色器模块失败!");
        }
        return shaderModule;
    }

    //插槽创建描述符
    void createDescriptorSetLayout() {
        VkDescriptorSetLayoutBinding uboLayoutBinding = {};
        uboLayoutBinding.binding = 0;
        uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uboLayoutBinding.descriptorCount = 1;
        uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        uboLayoutBinding.pImmutableSamplers = nullptr; // Optional

        //给纹理写描述符
        VkDescriptorSetLayoutBinding samplerLayoutBinding = {};
        samplerLayoutBinding.binding = 1;
        samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerLayoutBinding.descriptorCount = 1;
        samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        samplerLayoutBinding.pImmutableSamplers = nullptr; // Optional

        //槽位清单打包
        VkDescriptorSetLayoutBinding bindings[] = { uboLayoutBinding, samplerLayoutBinding };

        VkDescriptorSetLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 2;              // ① 1 → 2
        layoutInfo.pBindings = bindings;          // ② 指向数组（数组名=首地址）
        if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorSetLayout) != VK_SUCCESS) {
            throw std::runtime_error("创建描述符布局失败!");
        }
    }

    //创建图像管线
    void createGraphicsPipeline() {
        auto vertShaderCode = readFile("shaders/vert.spv");
        auto fragShaderCode = readFile("shaders/frag.spv");

        VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
        VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);

        //给顶点着色器模块填表
        VkPipelineShaderStageCreateInfo vertShaderStageInfo = {};
        vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertShaderStageInfo.module = vertShaderModule;
        vertShaderStageInfo.pName = "main";

        //片段着色器
        VkPipelineShaderStageCreateInfo fragShaderStageInfo = {};
        fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragShaderStageInfo.module = fragShaderModule;
        fragShaderStageInfo.pName = "main";

        //将顶点着色器和片段着色器打包
        VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo, fragShaderStageInfo };
        //顶点输入
        VkVertexInputBindingDescription vertexInputBindingDescription = {};
        vertexInputBindingDescription.binding = 0;
        vertexInputBindingDescription.stride = sizeof(float)*8; 
        vertexInputBindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attributeDescriptions[3] = {};
        // 位置：location 0，从顶点开头读 3 个 float
        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[0].offset = 0;
        // 颜色：location 1，从第 3 个 float 开始读 3 个 float
        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[1].offset = 3 * sizeof(float);   // = 12 字节
        // UV：location 2，从第 6 个 float 开始读 2 个 float
        attributeDescriptions[2].binding = 0;
        attributeDescriptions[2].location = 2;
        attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[2].offset = 6 * sizeof(float);   // = 24 字节

        VkPipelineVertexInputStateCreateInfo vertexInputState = {};
        vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputState.vertexBindingDescriptionCount = 1;
        vertexInputState.pVertexBindingDescriptions = &vertexInputBindingDescription;
        vertexInputState.vertexAttributeDescriptionCount = 3;
        vertexInputState.pVertexAttributeDescriptions = attributeDescriptions;

        //输入装配
        VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;
        //视口
        VkPipelineViewportStateCreateInfo viewportState = {};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        //光栅化
        VkPipelineRasterizationStateCreateInfo rasterizer = {};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE; //总开关
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL; //填充模式
        rasterizer.lineWidth = 1.0f; //线宽
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT; //背面剔除
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; //顺时针
        rasterizer.depthBiasEnable = VK_FALSE; //不使用深度偏移
        //多重采样
        VkPipelineMultisampleStateCreateInfo multisampling = {};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = msaaSamples;
        //颜色混合

            //子表
        VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT; //RGBA颜色
        colorBlendAttachment.blendEnable = VK_FALSE; //关闭混合
        //主表
        VkPipelineColorBlendStateCreateInfo colorBlending = {};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;                    // 不用逻辑运算混合
        colorBlending.logicOp = VK_LOGIC_OP_COPY;
        colorBlending.attachmentCount = 1;                         // 1 个颜色附件（交换链画布）
        colorBlending.pAttachments = &colorBlendAttachment;
        colorBlending.blendConstants[0] = 0.0f;                    // 混合常量（不用也要填）
        colorBlending.blendConstants[1] = 0.0f;
        colorBlending.blendConstants[2] = 0.0f;
        colorBlending.blendConstants[3] = 0.0f;


        //深度/模板测试
        VkPipelineDepthStencilStateCreateInfo depthStencil = {};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE; //开启深度测试
        depthStencil.depthWriteEnable = VK_TRUE; //开启深度写入
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS; //比较函数
        depthStencil.depthBoundsTestEnable = VK_FALSE; //不使用深度边界测试
        depthStencil.stencilTestEnable = VK_FALSE; //不使用模板测试

        // 动态状态：视口和裁剪的具体值不在管线里固定，每帧在命令缓冲里设置
        std::vector<VkDynamicState> dynamicStates = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };
        VkPipelineDynamicStateCreateInfo dynamicState = {};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        // 管线布局：着色器访问外部资源的"接线图"，现在空着
        VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;               // 没有描述符集
        pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 0;       // 没有推送常量

        if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("创建管线布局失败!");
        }

        //最后的主表
        VkGraphicsPipelineCreateInfo pipelineInfo = {};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;                          // 2 个着色器阶段
        pipelineInfo.pStages = shaderStages;                  // 顶点+片元阶段
        pipelineInfo.pVertexInputState = &vertexInputState;    // 顶点输入（空表）
        pipelineInfo.pInputAssemblyState = &inputAssembly;    // 输入装配
        pipelineInfo.pViewportState = &viewportState;         // 视口
        pipelineInfo.pRasterizationState = &rasterizer;       // 光栅化器
        pipelineInfo.pMultisampleState = &multisampling;      // 多重采样
        pipelineInfo.pColorBlendState = &colorBlending;       // 颜色混合
        pipelineInfo.pDynamicState = &dynamicState;           // 动态状态
        pipelineInfo.layout = pipelineLayout;                 // 管线布局
        pipelineInfo.pDepthStencilState = &depthStencil;        // 深度/模板测试
        pipelineInfo.renderPass = renderPass;                 // 渲染通道
        pipelineInfo.subpass = 0;                             // 用第 0 个子通道
        pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;     // 不做管线继承

        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {
            throw std::runtime_error("创建图形管线失败!");
        }

        vkDestroyShaderModule(device, fragShaderModule, nullptr);
        vkDestroyShaderModule(device, vertShaderModule, nullptr);

    }
    //创建渲染通道
    void createRenderPass() {
        //子表1：颜色附件（画布）的描述
        VkAttachmentDescription colorAttachment = {};
        colorAttachment.format = swapChainImageFormat;          // 画布格式 = 交换链选定的格式
        colorAttachment.samples = msaaSamples;        // 多重采样
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;   // 开始前：清空（清成背景色）
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE; // 结束后：保留像素（要显示）
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // 不用模板缓冲
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;        // 画布初始不管是什么
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;    // 最终交给显示器

        //子表1.1:深度附件的描述
        VkAttachmentDescription depthAttachment = {};
        depthAttachment.format = depthFormat;
        depthAttachment.samples = msaaSamples;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        //子表1.2:解析附件（交换链图：把 msaa 结果平均后写这）
        VkAttachmentDescription colorAttachmentResolve = {};
        colorAttachmentResolve.format = swapChainImageFormat;   // 和交换链同格式
        colorAttachmentResolve.samples = VK_SAMPLE_COUNT_1_BIT; // 单采样（最终屏幕图）
        colorAttachmentResolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // 不用清（会被覆盖）
        colorAttachmentResolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;    // 保留（要显示！）
        colorAttachmentResolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachmentResolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachmentResolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachmentResolve.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;  // 最终交给显示器

        //子表2：附件在子通道里的"用法引用"
        VkAttachmentReference colorAttachmentRef = {};
        colorAttachmentRef.attachment = 0;                      // 用第 0 张附件（上面那张）
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; // 当颜色画布用
        VkAttachmentReference depthAttachmentRef = {};
        depthAttachmentRef.attachment = 1;                              // 第 1 张附件（索引从 0 数）
        depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        // 采样新增：
        VkAttachmentReference resolveAttachmentRef = {};
        resolveAttachmentRef.attachment = 2;                    // 第 2 张附件（交换链图）
        resolveAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        //子表3：子通道（本次 1 个阶段）
        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; // 这是图形管线阶段
        subpass.colorAttachmentCount = 1;                       // 往 1 张颜色画布上画
        subpass.pColorAttachments = &colorAttachmentRef;
        subpass.pDepthStencilAttachment = &depthAttachmentRef;   // 这个子通道同时往深度画布写
        subpass.pResolveAttachments = &resolveAttachmentRef;

        VkAttachmentDescription attachments[3] = { colorAttachment, depthAttachment, colorAttachmentResolve };  // 数组装两张

        //主表
        VkRenderPassCreateInfo renderPassInfo = {};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 3;                     // 2 张附件
        renderPassInfo.pAttachments = attachments;        // 就是上面两张
        renderPassInfo.subpassCount = 1;                        // 1 个子通道
        renderPassInfo.pSubpasses = &subpass;

        if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
            throw std::runtime_error("创建渲染通道失败!");
        }

    }

    //帧缓冲
    void createFramebuffers() {
        swapChainFramebuffers.resize(swapChainImageViews.size());   // 准备 3 个空位

        for (size_t i = 0; i < swapChainImageViews.size(); i++) {
            //附件列表：这一帧画在哪张画布上（第 i 张图像视图）
            VkImageView attachments[] = {
                colorBufferView,               // 第 0 张：msaa 颜色图（绘制目标）
                depthImageView,                // 第 1 张：深度图
                swapChainImageViews[i]         // 第 2 张：交换链图（resolve 终点）
            };

            VkFramebufferCreateInfo framebufferInfo = {};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = renderPass;        // 遵守哪份合同
            framebufferInfo.attachmentCount = 3;            // 1 个附件
            framebufferInfo.pAttachments = attachments;     // 就是这张画布的视图
            framebufferInfo.width = swapChainExtent.width;  // 800
            framebufferInfo.height = swapChainExtent.height;// 600
            framebufferInfo.layers = 1;                     // 1 层

            if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &swapChainFramebuffers[i]) != VK_SUCCESS) {
                throw std::runtime_error("创建帧缓冲失败!");
            }
        }
    }
    //命令缓冲
        //命令池
    void createCommandPool() {
        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.pNext = nullptr;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily;
        VkResult result = vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("创建命令池失败!");
        }
    }
    //分配命令缓冲
    void createCommandBuffer() {
        commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);   // 3 个空位
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = MAX_FRAMES_IN_FLIGHT;  // 一次拿 3 本
        if (vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
            throw std::runtime_error("创建命令缓冲失败!");
        }
    }
    //录制函数 
    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; // 一次性订单：录完就执行

        if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
            throw std::runtime_error("开始录制失败!");
        }

        //① 开始渲染通道：绑第 imageIndex 个帧缓冲
        VkRenderPassBeginInfo renderPassInfo = {};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = renderPass;
        renderPassInfo.framebuffer = swapChainFramebuffers[imageIndex];
        renderPassInfo.renderArea.offset = { 0, 0 };              // 从画布左上角开始
        renderPassInfo.renderArea.extent = swapChainExtent;       // 覆盖整个 800×600

        VkClearValue clearValues[2] = {};//清屏 颜色+深度
        clearValues[0].color = { 0.0f, 0.0f, 0.0f, 1.0 };
        clearValues[1].depthStencil = { 1.0f, 0 };
        renderPassInfo.clearValueCount = 2;
        renderPassInfo.pClearValues = clearValues;

        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        //② 绑定管线（工厂配置上线）
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);

        //③ 视口：画布上"能看到"的区域
        VkViewport viewport = {};
        viewport.x = 0.0f;  viewport.y = 0.0f;
        viewport.width = static_cast<float>(swapChainExtent.width);   // 800
        viewport.height = static_cast<float>(swapChainExtent.height); // 600
        viewport.minDepth = 0.0f;  viewport.maxDepth = 1.0f;          // 深度范围 [0,1]
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

        //④ 裁剪矩形：像素允许写入的区域
        VkRect2D scissor = {};
        scissor.offset = { 0, 0 };
        scissor.extent = swapChainExtent;
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

        //⑤ 画！3 个顶点、1 个实例、从顶点 0 开始

        recordVertexBinding(commandBuffer);   // 绑定点：子类可注入
        recordDescriptorBinding(commandBuffer); //钩子: 子类可注入
        //vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        vkCmdDrawIndexed(commandBuffer, getIndexCount(), 1, 0, 0, 0);

        //⑥ 收工
        vkCmdEndRenderPass(commandBuffer);

        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            throw std::runtime_error("结束录制失败!");
        }
    }
    //渲染循环
    const int MAX_FRAMES_IN_FLIGHT = 3;  // 三帧流水线
    //栅栏
    void createSyncObjects() {
        imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
        renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
        inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);
        imagesInFlight.resize(swapChainImages.size(), VK_NULL_HANDLE);

        VkSemaphoreCreateInfo semaphoreInfo = {};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // 预置"已干完"——第一帧不用等

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS ||
                vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS ||
                vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS) {
                throw std::runtime_error("创建同步对象失败!");
            }
        }
    }

    //开始给交换链选择能力,格式，显示模式
            //采用固定窗口
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) {
        return capabilities.currentExtent;
    }
    //选择格式:优先sRGB格式
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats) {
        for (const auto& format : availableFormats) {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return format;
            }
        }
        return availableFormats[0]; //找不到sRGB格式就返回第一个
    }
    //选择显示模式:优先 MAILBOX 模式
    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes) {
        for (const auto& availablePresentMode : availablePresentModes) {
            if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
                return availablePresentMode;
            }
        }
        return VK_PRESENT_MODE_FIFO_KHR; //找不到 MAILBOX 模式就返回 FIFO 模式
    }



    void createInstance() {
        //检查校验层
        if (enableValidationLayers && !CheckValidationLayerSupport()) {
            throw std::runtime_error("强制依赖校验层, 但不支持校验层!");
        }

        VkApplicationInfo appInfo = {};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pNext = nullptr;
        appInfo.pApplicationName = "Hello Triangle";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "No Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo createInfo = {};
        createInfo.pApplicationInfo = &appInfo;
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pNext = nullptr;
        createInfo.flags = 0;

        auto extensions = getRequiredExtensions();
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        if (enableValidationLayers) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
            createInfo.ppEnabledLayerNames = validationLayers.data();
        }
        else {
            createInfo.enabledLayerCount = 0;
        }

        VkResult result = vkCreateInstance(&createInfo, nullptr, &instance);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("创建句柄失败!");
        }
    }

    bool isDeviceSuitable(VkPhysicalDevice device) {
        return true;   // 先全部"合适"——让流程跑通
    }


    void pickphysicalDevice() {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            throw std::runtime_error("没有找到GPU!");
        }
        std::vector<VkPhysicalDevice> devices(deviceCount); //将所有显卡句柄存入vector
        std::cout << "找到" << deviceCount << "块显卡" << std::endl;
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

        for (const auto& device : devices) {
            if (isDeviceSuitable(device)) {
                physicalDevice = device;
                break;
            }
        }

        if (physicalDevice == VK_NULL_HANDLE) {
            throw std::runtime_error("没有找到合适的显卡!");
        }

        VkPhysicalDeviceProperties deviceProperties;
        vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
        std::cout << "选中显卡: " << deviceProperties.deviceName << std::endl;

        queueFamilyIndices = findQueueFamilies(physicalDevice, surface);
        if (!queueFamilyIndices.isComplete()) {
            throw std::runtime_error("这张卡没有图形队列族!");
        }
        std::cout << "图形队列族编号: " << queueFamilyIndices.graphicsFamily << std::endl;
        std::cout << "显示队列族编号: " << queueFamilyIndices.presentFamily << std::endl;


    }

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface) {
        QueueFamilyIndices indices;
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());


        /////
        std::cout << "显卡队列族数量: " << queueFamilyCount << std::endl;
        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            std::cout << "族 " << i << ": "
                << ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? "图形+" : "")
                << ((queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) ? "计算+" : "")
                << ((queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT) ? "拷贝" : "")
                << " 队列数:" << queueFamilies[i].queueCount
                << std::endl;
        }
        /////
        //图形队列族检查
        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            if (queueFamilies[i].queueCount > 0 &&
                (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                indices.graphicsFamily = i;
            }
            //present 队列族检查
            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
            if (queueFamilies[i].queueCount > 0 && presentSupport) {
                indices.presentFamily = i;
            }
            if (indices.isComplete()) {
                break;
            }
        }
        return indices;
    }

    void createLogicalDevice() {
        //图形族 显示族 可能是同一个族，也可能不是同一个族 用set去重
        std::cout << "图像族编号: " << queueFamilyIndices.graphicsFamily << std::endl;
        std::cout << "显示族编号: " << queueFamilyIndices.presentFamily << std::endl;
        std::set<uint32_t> uniqueQueueFamilies = { (uint32_t)queueFamilyIndices.graphicsFamily, (uint32_t)queueFamilyIndices.presentFamily };


        //子表1：从图像族租用一条队列
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        for (uint32_t i : uniqueQueueFamilies) {
            VkDeviceQueueCreateInfo queueCreateInfo = {};
            queueCreateInfo.pNext = nullptr;
            queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = i;
            queueCreateInfo.queueCount = 1;
            float queuePriority = 1.0f;
            queueCreateInfo.pQueuePriorities = &queuePriority;
            queueCreateInfos.push_back(queueCreateInfo);
        }
        //子表2：告诉驱动我们要什么功能
        VkPhysicalDeviceFeatures deviceFeatures = {};
        //主表
        VkDeviceCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pNext = nullptr;
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pEnabledFeatures = &deviceFeatures;
        const std::vector<const char*> deviceExtensions = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME
        };
        createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();
        createInfo.enabledLayerCount = 0;

        VkResult result = vkCreateDevice(physicalDevice, &createInfo, nullptr, &device);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to create logical device!");
        }

        vkGetDeviceQueue(device, queueFamilyIndices.graphicsFamily, 0, &graphicsQueue);
        std::cout << "逻辑设备创建成功!" << std::endl;
    }

    //创建窗口表面
    void createSurface() {
        VkResult result = glfwCreateWindowSurface(instance, window, nullptr, &surface);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("创建窗口表面失败!");
        }
    }

    //初始化窗口
    void initWindow() {
        if (!glfwInit()) {
            throw std::runtime_error("初始化 GLFW 失败!");
        }
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        window = glfwCreateWindow(WIDTH, HEIGHT, "Vulkan", nullptr, nullptr);

        if (!window) {
            throw std::runtime_error("创建窗口失败!");
        }
    }

    //创建交换链
    void createSwapChain() {
        //查询 能力
        VkSurfaceCapabilitiesKHR capabilities;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities);

        //查询 支持的格式(由于数量未知  采用两段式查询)
        uint32_t formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> surfaceFormats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, surfaceFormats.data());

        //查询 支持的显示模式(由于数量未知  采用两段式查询)
        uint32_t presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, nullptr);
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, presentModes.data());

        std::cout << "交换链能力: " << std::endl;
        std::cout << "最小交换链长度: " << capabilities.minImageCount << std::endl;
        std::cout << "最大交换链长度: " << capabilities.maxImageCount << std::endl;
        std::cout << "当前图像宽度: " << capabilities.currentExtent.width << std::endl;
        std::cout << "当前图像高度: " << capabilities.currentExtent.height << std::endl;

        std::cout << "支持的格式: " << surfaceFormats.size() << std::endl;
        for (const auto& format : surfaceFormats) {
            std::cout << "格式: " << formatName(format.format)
                << " 颜色空间: " << format.colorSpace << std::endl;
        }
        std::cout << "支持的显示模式: " << presentModes.size() << std::endl;
        for (const auto& mode : presentModes) {
            std::cout << "呈现模式: " << presentModeName(mode) << std::endl;
        }
        //正式选择交换链参数
        VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(surfaceFormats);
        VkPresentModeKHR presentMode = chooseSwapPresentMode(presentModes);
        VkExtent2D extent = chooseSwapExtent(capabilities);
        swapChainImageFormat = surfaceFormat.format;   // 存进成员，供 createImageViews 用
        swapChainExtent = extent;                       // 同上（管线章节也要用）
        std::cout << "选择的交换链参数: " << std::endl;
        std::cout << "格式: " << formatName(surfaceFormat.format)
            << " 颜色空间: " << surfaceFormat.colorSpace << std::endl;
        std::cout << "呈现模式: " << presentModeName(presentMode) << std::endl;
        std::cout << "图像宽度: " << extent.width
            << " 图像高度: " << extent.height << std::endl;

        //开始给交换链填表
        imageCount = capabilities.minImageCount + 1; //至少要比最小值多一张图
        if (capabilities.minImageCount > 0 && imageCount > capabilities.maxImageCount) {
            imageCount = capabilities.maxImageCount;
        }//以免超过最大值
        VkSwapchainCreateInfoKHR createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = surface;                                    //  绑到哪个窗口
        createInfo.minImageCount = imageCount;                           //  画布数
        createInfo.imageFormat = surfaceFormat.format;                   //  像素格式
        createInfo.imageColorSpace = surfaceFormat.colorSpace;           //  色彩空间
        createInfo.imageExtent = extent;                                 //  画布尺寸
        createInfo.imageArrayLayers = 1;                                 //  1 层（非 VR）
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;     //  用途：当渲染目标
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;         //  单队列族专用
        createInfo.preTransform = capabilities.currentTransform;         //  跟随窗口变换
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;   //  不透明合成
        createInfo.presentMode = presentMode;                            //  呈现节奏
        createInfo.clipped = VK_TRUE;                                    //  被遮挡不画
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        VkResult result = vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapChain);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("创建交换链失败!");
        }
        std::cout << "交换链创建成功! 图像数量: " << imageCount << std::endl;
        //现在只是提交申请要
        //获取实际的图像数量
        uint32_t realImageCount = 0;
        vkGetSwapchainImagesKHR(device, swapChain, &realImageCount, nullptr);
        swapChainImages.resize(realImageCount);
        vkGetSwapchainImagesKHR(device, swapChain, &realImageCount, swapChainImages.data());
        std::cout << "交换链图像数: " << realImageCount << std::endl;
    }
    //给画布创建图像视图
    void createImageViews() {
        swapChainImageViews.resize(swapChainImages.size());
        for (size_t i = 0; i < swapChainImages.size(); i++) {
            // 创建图像视图的代码
            VkImageViewCreateInfo createInfo = {};
            createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            createInfo.image = swapChainImages[i];
            createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.format = swapChainImageFormat;        // ← 依赖①赋值
            createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            createInfo.subresourceRange.baseMipLevel = 0;
            createInfo.subresourceRange.levelCount = 1;
            createInfo.subresourceRange.baseArrayLayer = 0;
            createInfo.subresourceRange.layerCount = 1;

            VkResult result = vkCreateImageView(device, &createInfo, nullptr, &swapChainImageViews[i]);
            if (result != VK_SUCCESS) {
                throw std::runtime_error("创建图像视图失败!");
            }
        }
        std::cout << "图像视图创建完成: " << swapChainImageViews.size() << " 个" << std::endl;

    }
    //虚函数钩子 
    virtual void recordVertexBinding(VkCommandBuffer commandBuffer) {}
    //更新旋转顶点变换 钩子
    virtual void recordDescriptorBinding(VkCommandBuffer commandBuffer) {}
    virtual void updateUniformBufferHook() {}
    //虚函数获取索引
    virtual uint32_t getIndexCount() { return 0; }

    void initVulkan() {
        initWindow();
        createInstance();
        setupDebugCallback();
        createSurface();
        pickphysicalDevice();
        createLogicalDevice();
        vkGetDeviceQueue(device, queueFamilyIndices.presentFamily, 0, &presentQueue);
        msaaSamples = getMaxUsableSampleCount();   // 先问出采样次数（后面都用它）
        createSwapChain();
        createImageViews();
        createColorResources();      // ← 新增：先建 msaa 颜色图（后面 render pass 要用）
        createDepthResources();
        createRenderPass();
        createDescriptorSetLayout();//插槽清单
        createGraphicsPipeline();
        createFramebuffers();
        createCommandPool();
        createCommandBuffer();
        createSyncObjects();
        //创建窗口
    }

public:
    GLFWwindow* window = nullptr;//窗口句柄
    VkDevice device = VK_NULL_HANDLE; //逻辑设备句柄
    //栅栏
    void drawFrame() {
        //① 等上一帧干完（fence 放行才能重录订单）
        vkWaitForFences(device, 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);
        updateUniformBufferHook();
        //② 拿画布：从交换链要第 i 张（semaphore① 会发"画布可用"信号）
        uint32_t imageIndex;
        vkAcquireNextImageKHR(device, swapChain, UINT64_MAX,
            imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &imageIndex);

        //（进阶）这张画布正被另一帧占用 → 等那帧干完
        if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
            vkWaitForFences(device, 1, &imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
        }
        imagesInFlight[imageIndex] = inFlightFences[currentFrame];

        //③ 重录订单（先重置再录）
        vkResetFences(device, 1, &inFlightFences[currentFrame]);  // 栅栏降下，下一轮再等
        vkResetCommandBuffer(commandBuffers[currentFrame], 0);              // 重置当前帧这本
        recordCommandBuffer(commandBuffers[currentFrame], imageIndex);      // 录制当前帧这本

        //④ 提交执行：等"画布可用"，干完发"画完了"
        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        VkSemaphore waitSemaphores[] = { imageAvailableSemaphores[currentFrame] };
        VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT }; // 等到上色阶段才等
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffers[currentFrame];
        VkSemaphore signalSemaphores[] = { renderFinishedSemaphores[currentFrame] };
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        if (vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFences[currentFrame]) != VK_SUCCESS) {
            throw std::runtime_error("提交绘制失败!");
        }

        //⑤ 呈现：等"画完了"，把第 i 张画布交给显示器
        VkPresentInfoKHR presentInfo = {};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        VkSwapchainKHR swapChains[] = { swapChain };
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        presentInfo.pImageIndices = &imageIndex;
        vkQueuePresentKHR(presentQueue, &presentInfo);

        currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;  // 轮换
    }
    void mainLoop() {
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();   // 处理窗口事件
            drawFrame();        // 画一帧
        }
        vkDeviceWaitIdle(device);   // 窗口关了，等 GPU 所有活干完再销毁
    }
    VulkanTest3() {
        initVulkan();
    }
    ~VulkanTest3() {
        // ① 同步对象（最上层，最先毁）
        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            vkDestroySemaphore(device, imageAvailableSemaphores[i], nullptr);
            vkDestroySemaphore(device, renderFinishedSemaphores[i], nullptr);
            vkDestroyFence(device, inFlightFences[i], nullptr);
        }
        // ② 命令池（订单本跟着池子销毁）
        vkDestroyCommandPool(device, commandPool, nullptr);
        // ③ 帧缓冲（引用图像视图，先毁）
        for (auto framebuffer : swapChainFramebuffers) {
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
        // ④ 图形管线（引用渲染通道，先毁）
        vkDestroyPipeline(device, graphicsPipeline, nullptr);
        // ⑤ 管线布局
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);   // 父类创建，之前漏了
        // ⑥ 渲染通道
        vkDestroyRenderPass(device, renderPass, nullptr);
        //摧毁深度缓冲
        vkDestroyImageView(device, depthImageView, nullptr);
        vkFreeMemory(device, depthImageMemory, nullptr);
        vkDestroyImage(device, depthImage, nullptr);
        // ⑦ 图像视图 → ⑧ 交换链 → ⑨ 设备
        vkDestroyImageView(device, colorBufferView, nullptr);   // ① 视图先
        vkDestroyImage(device, colorBuffer, nullptr);           // ② 图像
        vkFreeMemory(device, colorBufferMemory, nullptr);       // ③ 显存
        for (auto imageView : swapChainImageViews) {
            vkDestroyImageView(device, imageView, nullptr);
        }
        vkDestroySwapchainKHR(device, swapChain, nullptr);
        // ⑩ 设备
        vkDestroyDevice(device, nullptr);
        // ⑪ 表面 → ⑫ 收件箱 → ⑬ 窗口 → ⑭ 实例
        vkDestroySurfaceKHR(instance, surface, nullptr);
        if (debugMessenger != VK_NULL_HANDLE) {
            DestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
        }
        glfwDestroyWindow(window);
        vkDestroyInstance(instance, nullptr);
        glfwTerminate();
    }


};


