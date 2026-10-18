#include "vulkanTest3.h"
#include <vector>
//glm官方拓展库::弧度
#include <glm/gtc/matrix_transform.hpp>
//材质贴图加载库
#define STB_IMAGE_IMPLEMENTATION    // 宏：只在这一处生成 stb 实现代码
#include <stb_image.h>
struct UniformBufferObject {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
};

class VulkanTest4 : public VulkanTest3 {
    // 这里写顶点缓冲的新代码（继承来的成员都能用）
protected:
    const std::vector<float> vertices = {//存下八个3d顶点
        // 前面 (+Z) 红
-0.5f,-0.5f, 0.5f,  1.0f,0.0f,0.0f, 0.0f,1.0f,
 0.5f,-0.5f, 0.5f,  1.0f,0.0f,0.0f, 1.0f,1.0f,
 0.5f, 0.5f, 0.5f,  1.0f,0.0f,0.0f, 1.0f,0.0f,
-0.5f, 0.5f, 0.5f,  1.0f,0.0f,0.0f, 0.0f,0.0f,
// 后面 (-Z) 青
-0.5f,-0.5f,-0.5f,  0.0f,1.0f,1.0f, 0.0f,1.0f,
 0.5f,-0.5f,-0.5f,  0.0f,1.0f,1.0f, 1.0f,1.0f,
 0.5f, 0.5f,-0.5f,  0.0f,1.0f,1.0f, 1.0f,0.0f,
-0.5f, 0.5f,-0.5f,  0.0f,1.0f,1.0f, 0.0f,0.0f,
// 右面 (+X) 黄
 0.5f,-0.5f, 0.5f,  1.0f,1.0f,0.0f, 0.0f,1.0f,
 0.5f,-0.5f,-0.5f,  1.0f,1.0f,0.0f, 1.0f,1.0f,
 0.5f, 0.5f,-0.5f,  1.0f,1.0f,0.0f, 1.0f,0.0f,
 0.5f, 0.5f, 0.5f,  1.0f,1.0f,0.0f, 0.0f,0.0f,
 // 左面 (-X) 绿
 -0.5f,-0.5f, 0.5f,  0.0f,1.0f,0.0f, 0.0f,1.0f,
 -0.5f,-0.5f,-0.5f,  0.0f,1.0f,0.0f, 1.0f,1.0f,
 -0.5f, 0.5f,-0.5f,  0.0f,1.0f,0.0f, 1.0f,0.0f,
 -0.5f, 0.5f, 0.5f,  0.0f,1.0f,0.0f, 0.0f,0.0f,
 // 上面 (+Y) 蓝
 -0.5f, 0.5f, 0.5f,  0.0f,0.0f,1.0f, 0.0f,1.0f,
  0.5f, 0.5f, 0.5f,  0.0f,0.0f,1.0f, 1.0f,1.0f,
  0.5f, 0.5f,-0.5f,  0.0f,0.0f,1.0f, 1.0f,0.0f,
 -0.5f, 0.5f,-0.5f,  0.0f,0.0f,1.0f, 0.0f,0.0f,
 // 下面 (-Y) 紫
 -0.5f,-0.5f, 0.5f,  1.0f,0.0f,1.0f, 0.0f,1.0f,
  0.5f,-0.5f, 0.5f,  1.0f,0.0f,1.0f, 1.0f,1.0f,
  0.5f,-0.5f,-0.5f,  1.0f,0.0f,1.0f, 1.0f,0.0f,
 -0.5f,-0.5f,-0.5f,  1.0f,0.0f,1.0f, 0.0f,0.0f,
    };
    const std::vector<uint16_t> indices = { //顶点绘制顺序 后续会使用一个函数自动化运行
        0,1,2, 2,3,0,         // 前面（顶点 0-3）
        4,6,5, 6,4,7,         // 后面（顶点 4-7）
        8,9,10, 10,11,8,      // 右面（顶点 8-11）
        12,14,13, 14,12,15,   // 左面（顶点 12-15）
        16,17,18, 18,19,16,   // 上面（顶点 16-19）
        20,22,21, 22,20,23    // 下面（顶点 20-23）
    };//（uint16_t 够用，因为最大才 7）


        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;

        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;    // 描述符池：仓库
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;      // 描述符集：登记结果

        //3D使用的变量
        VkBuffer indexBuffer;
        VkDeviceMemory indexBufferMemory;
        //用于贴图的图像视图
        VkImageView textureImageView;
        //采样器
        VkSampler textureSampler;
        uint32_t mipLevels;     // ← 新增：这张图的金字塔级数
        
        //缓存更新函数 每秒更新角度
        void updateUniformBuffer() {
            //时间来源
            float time = glfwGetTime();
            UniformBufferObject ubo = {};

            ubo.model = glm::rotate(glm::mat4(1.0f), (float)time, glm::vec3(0, 0, 1));      // 转
            ubo.view = glm::lookAt(glm::vec3(2, 2, 2), glm::vec3(0, 0, 0), glm::vec3(0, 0, 1)); // 镜头
            ubo.proj = glm::perspective(glm::radians(45.0f),
                swapChainExtent.width / (float)swapChainExtent.height,
                0.1f, 10.0f);
            ubo.proj[1][1] *= -1;   // Vulkan Y 翻转（你懂的）
            void* data;
            vkMapMemory(device, uniformBufferMemory, 0, sizeof(ubo), 0, &data);
            memcpy(data, &ubo, sizeof(ubo));
            vkUnmapMemory(device, uniformBufferMemory);
        }

        //通用函数 会被反复用
        void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
            VkMemoryPropertyFlags properties,
            VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
            // 表1：壳规格（size/usage 由调用方指定）
            VkBufferCreateInfo bufferInfo = {};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = size;
            bufferInfo.usage = usage;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
                throw std::runtime_error("创建缓冲失败!");
            }

            // 问需求
            VkMemoryRequirements memRequirements;
            vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

            // 表2：内存分配（properties 由调用方指定要求）
            VkMemoryAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.allocationSize = memRequirements.size;
            allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);
            if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
                throw std::runtime_error("分配显存失败!");
            }

            vkBindBufferMemory(device, buffer, bufferMemory, 0);


        }
        //让GPU干活
        void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
            // 分配一条临时命令缓冲（用继承来的 commandPool）
            VkCommandBufferAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandPool = commandPool;
            allocInfo.commandBufferCount = 1;

            VkCommandBuffer commandBuffer;
            vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

            // 开始录制（一次性用途）
            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(commandBuffer, &beginInfo);

            // 录一条拷贝指令
            VkBufferCopy copyRegion = {};
            copyRegion.size = size;
            vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

            vkEndCommandBuffer(commandBuffer);

            // 提交到图形队列，等待执行完
            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commandBuffer;
            vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
            vkQueueWaitIdle(graphicsQueue);   // 阻塞等拷贝完成

            // 用完释放
            vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
        }
        //uniform 缓冲
        VkBuffer uniformBuffer = VK_NULL_HANDLE;
        VkDeviceMemory uniformBufferMemory = VK_NULL_HANDLE;
        //纹理实现
        VkImage textureImage;//纹理图像句柄
        VkDeviceMemory textureImageMemory;//用于纹理的显存

        void createUniformBuffer() {
            VkDeviceSize bufferSize = sizeof(UniformBufferObject);
            createBuffer(bufferSize,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,  // 用途：uniform 缓冲
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                uniformBuffer, uniformBufferMemory);
        }

        //
        void createDescriptorPool() {
            //表1: 描述符池规格
            VkDescriptorPoolSize poolSize = {};
            poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            poolSize.descriptorCount = 1;

            VkDescriptorPoolSize poolSize2 = {};
            poolSize2.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            poolSize2.descriptorCount = 1;

            VkDescriptorPoolSize poolSizes[] = { poolSize, poolSize2 };

            VkDescriptorPoolCreateInfo poolInfo = {};
            poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolInfo.poolSizeCount = 2;
            poolInfo.pPoolSizes = poolSizes;
            poolInfo.maxSets = 1;

            if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
                    throw std::runtime_error("创建描述符池失败!");
            }
        }
        //从池里分配描述符集
        void createDescriptorSet() {
            // 按 C 步的清单，从池里分配一个描述符集
            VkDescriptorSetLayout layouts[] = { descriptorSetLayout };
            VkDescriptorSetAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool = descriptorPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts = layouts;
            if (vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet) != VK_SUCCESS) {
                throw std::runtime_error("分配描述符集失败!");
            }

            // 登记：把 uniform 缓冲写进 binding 0（正式"插电"）
            VkDescriptorBufferInfo bufferInfo = {};
            bufferInfo.buffer = uniformBuffer;        // 哪个缓冲
            bufferInfo.offset = 0;                    // 从第 0 字节
            bufferInfo.range = sizeof(UniformBufferObject);     // 多长（192 字节）

            VkWriteDescriptorSet descriptorWrite = {};
            descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrite.dstSet = descriptorSet;       // 写到哪个集
            descriptorWrite.dstBinding = 0;               // 暗号：binding 0
            descriptorWrite.dstArrayElement = 0;          // 数组下标（无数组就 0）
            descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptorWrite.descriptorCount = 1;
            descriptorWrite.pBufferInfo = &bufferInfo;    // 指向"缓冲规格"表
            
            //纹理实现
            VkDescriptorImageInfo imageInfo = {};
            imageInfo.sampler = textureSampler; // 纹理采样器
            imageInfo.imageView = textureImageView; // 纹理视图
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; // 纹理布局

            VkWriteDescriptorSet descriptorWriteImage = {};
            descriptorWriteImage.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWriteImage.dstSet = descriptorSet;
            descriptorWriteImage.dstBinding = 1;
            descriptorWriteImage.dstArrayElement = 0;
            descriptorWriteImage.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWriteImage.descriptorCount = 1;
            descriptorWriteImage.pImageInfo = &imageInfo;

            VkWriteDescriptorSet descriptorWrites[] = { descriptorWrite, descriptorWriteImage };
            vkUpdateDescriptorSets(device, 2, descriptorWrites, 0, nullptr);


        }

        //3D使用的顶点缓存
        void createIndexBuffer() {
            VkDeviceSize bufferSize = sizeof(indices[0]) * indices.size();

            // staging：CPU 能写的内存
            VkBuffer stagingBuffer;
            VkDeviceMemory stagingBufferMemory;
            createBuffer(bufferSize,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,              // 用途：拷贝的"源"
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                stagingBuffer, stagingBufferMemory);

            // CPU 直接写入 staging（拿到显存地址当普通指针用）
            void* data;
            vkMapMemory(device, stagingBufferMemory, 0, bufferSize, 0, &data);
            memcpy(data, indices.data(), (size_t)bufferSize);
            vkUnmapMemory(device, stagingBufferMemory);

            // 最终缓冲：DEVICE_LOCAL（GPU 最快）
            createBuffer(bufferSize,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,  // 用途：拷贝的"目标" + 顶点数据(此处发生更改  改为3D)
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                indexBuffer, indexBufferMemory);

            // GPU 执行拷贝：staging → 最终
            copyBuffer(stagingBuffer, indexBuffer, bufferSize);

            // staging 用完即毁（它的使命结束了）
            vkDestroyBuffer(device, stagingBuffer, nullptr);
            vkFreeMemory(device, stagingBufferMemory, nullptr);
        }

        void createVertexBuffer() {
            VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();

            // staging：CPU 能写的内存
            VkBuffer stagingBuffer;
            VkDeviceMemory stagingBufferMemory;
            createBuffer(bufferSize,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,              // 用途：拷贝的"源"
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                stagingBuffer, stagingBufferMemory);

            // CPU 直接写入 staging（拿到显存地址当普通指针用）
            void* data;
            vkMapMemory(device, stagingBufferMemory, 0, bufferSize, 0, &data);
            memcpy(data, vertices.data(), (size_t)bufferSize);
            vkUnmapMemory(device, stagingBufferMemory);

            // 最终缓冲：DEVICE_LOCAL（GPU 最快）
            createBuffer(bufferSize,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,  // 用途：拷贝的"目标" + 顶点数据
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                vertexBuffer, vertexBufferMemory);

            // GPU 执行拷贝：staging → 最终
            copyBuffer(stagingBuffer, vertexBuffer, bufferSize);

            // staging 用完即毁（它的使命结束了）
            vkDestroyBuffer(device, stagingBuffer, nullptr);
            vkFreeMemory(device, stagingBufferMemory, nullptr);
        }
        
        void printMemoryTypes() {
            VkPhysicalDeviceMemoryProperties memProps;
            vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
            for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
                auto flags = memProps.memoryTypes[i].propertyFlags;
                std::cout << "类型 " << i << ": ";
                if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)  std::cout << "DEVICE_LOCAL ";
                if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)  std::cout << "HOST_VISIBLE ";
                if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) std::cout << "HOST_COHERENT ";
                if (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)   std::cout << "HOST_CACHED ";
                std::cout << std::endl;
            }
        }
        //贴图加载函数=>升级为createTextureImage
        void createTextureImage() {
            // 接收图片尺寸的变量
            int texWidth, texHeight, texChannels;

            // 解码：路径 + 输出宽/高/通道 + 强制 4 通道 RGBA
            stbi_uc* pixels = stbi_load("textures/grass_side.png",
                &texWidth, &texHeight, &texChannels,
                STBI_rgb_alpha);
            //金字塔计算
            mipLevels = static_cast<uint32_t>(std::floor(std::log2((std::max)(texWidth, texHeight)))) + 1;
            if (!pixels) {
                std::cerr << "stb失败原因: " << stbi_failure_reason() << std::endl;
                throw std::runtime_error("加载纹理失败!");
            }

            //在显存开区域
            createImage(texWidth, texHeight,
                VK_FORMAT_R8G8B8A8_SRGB,
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |       // 能当"拷贝源"（blit 要读它）
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                mipLevels, VK_SAMPLE_COUNT_1_BIT,
                textureImage, textureImageMemory);
             
            //将解码后的图放进gpu:staging
            VkDeviceSize imageSize = texWidth * texHeight * 4;
            
            VkBuffer stagingBuffer;
            VkDeviceMemory stagingBufferMemory;
            createBuffer(imageSize,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,  //和顶点stageing一样写法
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                stagingBuffer, stagingBufferMemory);
            //cpu写入staging
            void* data;
            vkMapMemory(device, stagingBufferMemory, 0, imageSize, 0, &data);
            memcpy(data, pixels, (size_t)imageSize);
            vkUnmapMemory(device, stagingBufferMemory);
            //图片像素拷贝完毕 删除后，释放 CPU 内存
            stbi_image_free(pixels);
            //将staging拷贝到gpu
            // 
            // 拷贝前：解锁到"接收拷贝"状态
            transitionImageLayout(textureImage, VK_FORMAT_R8G8B8A8_SRGB,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,mipLevels);
            copyBufferToImage(stagingBuffer, textureImage, texWidth, texHeight);
            // 拷贝后：上锁到"shader只读"状态
            //transitionImageLayout(textureImage, VK_FORMAT_R8G8B8A8_SRGB,
            //    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, mipLevels);
            generateMipmaps(textureImage, VK_FORMAT_R8G8B8A8_SRGB,
                texWidth, texHeight, mipLevels);   // ← 换成它

            // staging 用完即毁（它的使命结束了）
            vkDestroyBuffer(device, stagingBuffer, nullptr);
            vkFreeMemory(device, stagingBufferMemory, nullptr);
        }

        //将stagign拷贝到gpu
        void copyBufferToImage(VkBuffer srcBuffer, VkImage dstImage, uint32_t width, uint32_t height) {
            //分配一条命令缓冲
            VkCommandBufferAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandPool = commandPool;
            allocInfo.commandBufferCount = 1;

            VkCommandBuffer commandBuffer;
            vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

            //开始录制 
            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(commandBuffer, &beginInfo);

            //录制一条 命令：拷贝缓冲到图像
            VkBufferImageCopy region = {};
            region.bufferOffset = 0;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = { 0, 0, 0 };
            region.imageExtent = { width,height,1 };
            vkCmdCopyBufferToImage(commandBuffer, srcBuffer, dstImage,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            vkEndCommandBuffer(commandBuffer);



            //提交给图形队列
            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commandBuffer;
            vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
            vkQueueWaitIdle(graphicsQueue);
            //用完释放 
            vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);

        }

        //图的转换函数:将图的状态变为专供shader采样
        void transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout,uint32_t mipLevels) {
            VkCommandBufferAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandPool = commandPool;
            allocInfo.commandBufferCount = 1;

            VkCommandBuffer comandBuffer;
            vkAllocateCommandBuffers(device, &allocInfo, &comandBuffer);

            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(comandBuffer, &beginInfo);

            //管线屏障 布局转换的真身
            VkImageMemoryBarrier barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = mipLevels;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;      // ← 补这行
            
            //两种情况 从未定义=>接受拷贝
            VkPipelineStageFlags srcStage, dstStage;
            if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
                barrier.srcAccessMask = 0;                              // 之前没做过事
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;   // 之后要写
                srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            }//接受拷贝=>shader可读
            else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;   // 之前写过
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;      // 之后要读
                srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            }
            else {
                throw std::invalid_argument("无法转换! 转换源头和目标不合法");
            }

            vkCmdPipelineBarrier(comandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            vkEndCommandBuffer(comandBuffer);
            //提交给gpu
            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &comandBuffer;
            vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
            vkQueueWaitIdle(graphicsQueue);
            //用完释放
            vkFreeCommandBuffers(device, commandPool, 1, &comandBuffer);
        }
        //生成mipLevels
        void generateMipmaps(VkImage image, VkFormat imageFormat,
            int32_t texWidth, int32_t texHeight, uint32_t mipLevels) {

            // 段1：查显卡是否支持"线性缩小"（R8G8B8A8_SRGB 几乎都支持，但好习惯要查）
            VkFormatProperties props;
            vkGetPhysicalDeviceFormatProperties(physicalDevice, imageFormat, &props);
            if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
                throw std::runtime_error("显卡不支持线性缩小!");
            }

            // 段2：一次性命令缓冲（和 transitionImageLayout 一样的骨架）
            VkCommandBufferAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandPool = commandPool;
            allocInfo.commandBufferCount = 1;
            VkCommandBuffer comandBuffer;
            vkAllocateCommandBuffers(device, &allocInfo, &comandBuffer);

            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(comandBuffer, &beginInfo);

            // 段3：逐级缩小（核心循环）
            int32_t srcWidth = texWidth, srcHeight = texHeight;   // 当前"源"尺寸
            for (uint32_t i = 1; i < mipLevels; i++) {
                int32_t dstWidth = (std::max)(1, srcWidth / 2);      // ← 加括号
                int32_t dstHeight = (std::max)(1, srcHeight / 2);

                // 3-1：上一级 mip(i-1) 从"接收区"解锁成"读取区"（blit 要读它）
                VkImageMemoryBarrier barrier = {};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image;
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.baseMipLevel = i - 1;    // ← 指到"上一级"
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount = 1;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(comandBuffer,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &barrier);

                // 3-2：本级 mip(i) 从"垃圾"解锁成"接收区"
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.subresourceRange.baseMipLevel = i;        // ← 指到"本级"
                barrier.srcAccessMask = 0;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(comandBuffer,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &barrier);

                // 3-3：缩放拷贝：mip(i-1) → mip(i)（LINEAR = 平均缩小）
                VkImageBlit blit = {};
                blit.srcOffsets[0] = { 0, 0, 0 };
                blit.srcOffsets[1] = { srcWidth, srcHeight, 1 };
                blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.srcSubresource.mipLevel = i - 1;
                blit.srcSubresource.baseArrayLayer = 0;
                blit.srcSubresource.layerCount = 1;
                blit.dstOffsets[0] = { 0, 0, 0 };
                blit.dstOffsets[1] = { dstWidth, dstHeight, 1 };
                blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.dstSubresource.mipLevel = i;
                blit.dstSubresource.baseArrayLayer = 0;
                blit.dstSubresource.layerCount = 1;
                vkCmdBlitImage(comandBuffer,
                    image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,   // 源图+它的布局
                    image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,   // 目标图+它的布局
                    1, &blit, VK_FILTER_LINEAR);                   // 线性过滤（平均）

                // 3-4：生成完，上一级"上锁可读"（shader 以后要采样它）
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.subresourceRange.baseMipLevel = i - 1;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(comandBuffer,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &barrier);

                srcWidth = dstWidth;    // 源变成刚生成的级，继续往下缩
                srcHeight = dstHeight;
            }

            // 段4：最后一级生成完，也"上锁可读"
            VkImageMemoryBarrier barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = mipLevels - 1;  // 最后一级
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(comandBuffer,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);

            // 提交+等待（同 copyBufferToImage 结尾那段）
            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &comandBuffer;
            vkEndCommandBuffer(comandBuffer);                         // ← 录制结束（漏了这行！）
            vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
            vkQueueWaitIdle(graphicsQueue);
            vkFreeCommandBuffers(device, commandPool, 1, &comandBuffer);
        }

        //贴图用的图像视图
        void createTextureImageView() {
            VkImageViewCreateInfo createInfo = {};
            createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            createInfo.image = textureImage;
            createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.format = VK_FORMAT_R8G8B8A8_SRGB;        // ← 依赖①赋值
            createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            createInfo.subresourceRange.baseMipLevel = 0;
            createInfo.subresourceRange.levelCount = mipLevels;
            createInfo.subresourceRange.baseArrayLayer = 0;
            createInfo.subresourceRange.layerCount = 1;

            VkResult result = vkCreateImageView(device, &createInfo, nullptr, &textureImageView);
            if (result != VK_SUCCESS) {
                throw std::runtime_error("创建贴图视图失败!");
            }
        }
        //采样器
        void createTextureSampler() {
            VkSamplerCreateInfo createInfo = {};
            createInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            createInfo.magFilter = VK_FILTER_LINEAR;
            createInfo.minFilter = VK_FILTER_LINEAR;
            createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            createInfo.anisotropyEnable = VK_FALSE;        // 各向异性先关（性能）
            createInfo.maxLod = mipLevels;
            createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            createInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
            createInfo.unnormalizedCoordinates = VK_FALSE;

            if (vkCreateSampler(device, &createInfo, nullptr, &textureSampler) != VK_SUCCESS) {
                throw std::runtime_error("创建采样器失败!");
            }
        }

        void recordVertexBinding(VkCommandBuffer commandBuffer) override {
            VkBuffer vertexBuffers[] = { vertexBuffer };   // 绑哪个缓冲
            VkDeviceSize offsets[] = { 0 };                // 从缓冲第 0 字节开始
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
            //                                    ↑ binding=0 —— 和 D 步描述对暗号！
            vkCmdBindIndexBuffer(commandBuffer, indexBuffer, 0, VK_INDEX_TYPE_UINT16);//
        };

        uint32_t getIndexCount() override { return indices.size(); }

        void recordDescriptorBinding(VkCommandBuffer commandBuffer) override{
            vkCmdBindDescriptorSets(
                commandBuffer,                          // ① 订单本
                VK_PIPELINE_BIND_POINT_GRAPHICS,        // ② 图形管线
                pipelineLayout,                         // ③ 插槽清单（继承自父类，C步接好的）
                0,                                      // ④ 从第0个集开始
                1,                                      // ⑤ 绑1个集
                &descriptorSet,                         // ⑥ 登记表（D步做的）
                0,                                      // ⑦ 无动态偏移
                nullptr);                               // ⑧ 无动态偏移数组
        }

        void updateUniformBufferHook() override {
            updateUniformBuffer();
        }

public:
    VulkanTest4() {
        printMemoryTypes();   // 先看菜单
        createTextureImage();   // 先验证加载（后续步骤会替换/扩展它）
        createTextureImageView();//
        createTextureSampler();
        createVertexBuffer();   // 父类 initVulkan 已跑完，这里加装顶点缓冲
        createIndexBuffer();
        createUniformBuffer();
        createDescriptorPool();
        createDescriptorSet();
    }

    ~VulkanTest4() {
        vkDestroyBuffer(device, vertexBuffer, nullptr);
        vkFreeMemory(device, vertexBufferMemory, nullptr);
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);   // 描述符集随池回收
        vkDestroyBuffer(device, uniformBuffer, nullptr);
        vkDestroyBuffer(device, indexBuffer, nullptr);
        vkFreeMemory(device, indexBufferMemory, nullptr);
        vkFreeMemory(device, uniformBufferMemory, nullptr);

        // ===== 纹理四件套（按"引用者先、被引用者后"的顺序） =====
        vkDestroySampler(device, textureSampler, nullptr);      // ① 采样器（引用视图）
        vkDestroyImageView(device, textureImageView, nullptr);  // ② 视图（引用图像）
        vkDestroyImage(device, textureImage, nullptr);          // ③ 图像本体
        vkFreeMemory(device, textureImageMemory, nullptr);      // ④ 图像显存
    }
};

int main() {
    SetConsoleOutputCP(CP_UTF8);
    VulkanTest4 test;
    test.mainLoop();
    return 0;
}
