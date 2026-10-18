#pragma once
// util 工具层聚合头：一行引入全部工具（按类型分文件）。
// 无状态自由函数，只依赖参数，不碰类成员。
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "DeviceUtils.hpp"
#include "SwapchainUtils.hpp"
#include "ResourcesUtils.hpp"
#include "CommandUtils.hpp"
#include "ShaderUtils.hpp"
