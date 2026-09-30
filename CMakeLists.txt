cmake_minimum_required(VERSION 3.20)

# 自研方块沙盒引擎 —— 单机原型（客户端+服务端同进程）
# 对应《沙盒引擎架构设计文档.md》7.1 原型阶段最小核心
project(sandbox_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# 自动收集 src 下所有源文件与头文件（新增文件后需重新配置一次）
file(GLOB_RECURSE SANDBOX_SOURCES CONFIGURE_DEPENDS src/*.cpp)
file(GLOB_RECURSE SANDBOX_HEADERS CONFIGURE_DEPENDS src/*.hpp)

# 单机原型：客户端与服务端编译进同一个进程，一个可执行目标
add_executable(sandbox_engine
    ${SANDBOX_SOURCES}
    ${SANDBOX_HEADERS}
)

target_include_directories(sandbox_engine PRIVATE src)

# ---- 第三方依赖（vcpkg manifest 模式，见 vcpkg.json）----
# configure 时通过 vcpkg.cmake toolchain 提供（CMakePresets 已接）：
#   -DCMAKE_TOOLCHAIN_FILE=E:/VS2026/VC/vcpkg/scripts/buildsystems/vcpkg.cmake
# glm：GL 数学库（header-only）——向量/矩阵/四元数，渲染计算基石
find_package(glm CONFIG REQUIRED)
# glfw3：跨平台窗口 + 输入 + 窗口表面（Vulkan 对接用）
find_package(glfw3 CONFIG REQUIRED)
# volk：Vulkan meta-loader（运行时动态加载 vulkan-1.dll，免装 LunarG SDK）
find_package(volk CONFIG REQUIRED)
# vulkan-headers：Vulkan C API 头文件（与 volk 搭配，无需 SDK 的 .lib）
find_package(VulkanHeaders CONFIG REQUIRED)
# shaderc：GLSL → SPIR-V 运行时编译（免 LunarG SDK 的 glslangValidator 命令行）
find_package(unofficial-shaderc CONFIG REQUIRED)

# 显式链接：模拟三轨不依赖图形库；渲染层用 GLFW 窗口 + Vulkan + shaderc 着色器编译。
target_link_libraries(sandbox_engine PRIVATE
    glm::glm
    glfw
    volk::volk
    Vulkan::Headers
    unofficial::shaderc::shaderc
)

if(MSVC)
    # 源码含中文注释，强制 cl 以 UTF-8 读取（避免 GBK 代码页误解析触发 C4819/语法错）
    target_compile_options(sandbox_engine PRIVATE /utf-8)
    # 严格告警：把警告当错误，尽早暴露跨线程/未初始化隐患
    target_compile_options(sandbox_engine PRIVATE /W4 /WX /permissive-)
    # 注：Debug 暂不启用 MSVC ASan（/fsanitize=address），
    # 它在该工具集上与标准库头不兼容且不抓数据竞争；
    # 竞态排查后续靠 VS 并行堆栈/线程窗口 + 数据断点。
endif()

# ---- 测试（后期启用）----
# enable_testing()
# add_subdirectory(tests)
