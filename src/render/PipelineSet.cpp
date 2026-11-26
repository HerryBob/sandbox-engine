#include "render/PipelineSet.hpp"

#include "render/VulkanContext.hpp"   // 只为 Vertex 顶点布局定义（管线要它）

#include <sstream>
#include <vector>

#include "core/Log.hpp"
#include "render/utils/CommandUtils.hpp"
#include "render/utils/ResourcesUtils.hpp"
#include "render/utils/ShaderUtils.hpp"

namespace engine::render {

bool PipelineSet::initAttachmentsAndPass(VkDevice device, VkPhysicalDevice physicalDevice,
                                         VkFormat swapchainFormat, VkExtent2D extent,
                                         VkSampleCountFlagBits msaaSamples, VkFormat depthFormat) {
    device_ = device;
    physicalDevice_ = physicalDevice;
    swapchainFormat_ = swapchainFormat;
    swapchainExtent_ = extent;
    msaaSamples_ = msaaSamples;
    depthFormat_ = depthFormat;
    if (!createRenderPass()) return false;
    if (!createDepthResources()) return false;
    if (!createColorResources()) return false;
    return true;
}

bool PipelineSet::initPipeline(VkDescriptorSetLayout descLayout) {
    descLayout_ = descLayout;   // 必须在 createUniformObjects 之后调用，否则布局是空的
    if (descLayout_ == VK_NULL_HANDLE) {
        ENGINE_LOG_ERROR("Vulkan", "PipelineSet::initPipeline：descLayout 为空（调用顺序错误）");
        return false;
    }
    return createPipeline();
}

// 交换链尺寸变化：附件按新尺寸重建（renderPass 与管线不依赖尺寸，保持不动）
bool PipelineSet::recreateAttachments(VkExtent2D extent) {
    swapchainExtent_ = extent;
    if (depthImage_) { vkDestroyImage(device_, depthImage_, nullptr); depthImage_ = VK_NULL_HANDLE; }
    if (depthImageMemory_) { vkFreeMemory(device_, depthImageMemory_, nullptr); depthImageMemory_ = VK_NULL_HANDLE; }
    if (depthImageView_) { vkDestroyImageView(device_, depthImageView_, nullptr); depthImageView_ = VK_NULL_HANDLE; }
    if (colorBuffer_) { vkDestroyImage(device_, colorBuffer_, nullptr); colorBuffer_ = VK_NULL_HANDLE; }
    if (colorBufferMemory_) { vkFreeMemory(device_, colorBufferMemory_, nullptr); colorBufferMemory_ = VK_NULL_HANDLE; }
    if (colorBufferView_) { vkDestroyImageView(device_, colorBufferView_, nullptr); colorBufferView_ = VK_NULL_HANDLE; }
    return createDepthResources() && createColorResources();
}

void PipelineSet::shutdown() {
    if (pipeline_) { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (cutoutPipeline_) { vkDestroyPipeline(device_, cutoutPipeline_, nullptr); cutoutPipeline_ = VK_NULL_HANDLE; }
    if (pipelineLayout_) { vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr); pipelineLayout_ = VK_NULL_HANDLE; }
    if (depthImageView_) { vkDestroyImageView(device_, depthImageView_, nullptr); depthImageView_ = VK_NULL_HANDLE; }
    if (depthImage_) { vkDestroyImage(device_, depthImage_, nullptr); depthImage_ = VK_NULL_HANDLE; }
    if (depthImageMemory_) { vkFreeMemory(device_, depthImageMemory_, nullptr); depthImageMemory_ = VK_NULL_HANDLE; }
    if (colorBufferView_) { vkDestroyImageView(device_, colorBufferView_, nullptr); colorBufferView_ = VK_NULL_HANDLE; }
    if (colorBuffer_) { vkDestroyImage(device_, colorBuffer_, nullptr); colorBuffer_ = VK_NULL_HANDLE; }
    if (colorBufferMemory_) { vkFreeMemory(device_, colorBufferMemory_, nullptr); colorBufferMemory_ = VK_NULL_HANDLE; }
    if (renderPass_) { vkDestroyRenderPass(device_, renderPass_, nullptr); renderPass_ = VK_NULL_HANDLE; }
}
bool PipelineSet::createRenderPass() {
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
bool PipelineSet::createDepthResources() {
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
bool PipelineSet::createColorResources() {
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
bool PipelineSet::createPipeline() {
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
    // 相机矩阵 viewProj 也一并推（总共 128 字节 = 2 个 mat4，正好是规范保证的下限）。
    // 这样每个命令缓冲自带一份相机矩阵，不再跨帧共享 UBO。
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(glm::mat4) * 2;
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
} // namespace engine::render
