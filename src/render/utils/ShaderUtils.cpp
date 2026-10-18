#include "ShaderUtils.hpp"
#include "core/Log.hpp"

#include <cstddef>
#include <fstream>
#include <sstream>
#include <vector>

namespace engine::render::util {

// 读 .spv 文件 → VkShaderModule
VkShaderModule loadShaderModule(VkDevice dev, const char* spvPath) {
    std::ifstream f(spvPath, std::ios::binary | std::ios::ate);
    if (!f) {
        std::ostringstream os;
        os << "[Vulkan] 打不开着色器文件: " << spvPath;
        LOG_ERROR(os.str());
        return VK_NULL_HANDLE;
    }
    const std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<char> bytes(size);
    if (size > 0) f.read(bytes.data(), size);

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = static_cast<std::size_t>(size);
    createInfo.pCode = reinterpret_cast<const uint32_t*>(bytes.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &createInfo, nullptr, &mod) != VK_SUCCESS) {
        std::ostringstream os;
        os << "[Vulkan] vkCreateShaderModule 失败: " << spvPath;
        LOG_ERROR(os.str());
        return VK_NULL_HANDLE;
    }
    return mod;
}

} // namespace engine::render::util
