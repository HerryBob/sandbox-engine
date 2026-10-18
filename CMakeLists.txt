cmake_minimum_required(VERSION 3.20)

# MC 风格体素引擎 —— 由《Vulkan编程指南》学习代码（vulkanTest3/vulkanTest4）演进而来
#
# 依赖策略（离线、可控，不依赖 vcpkg/网络）：
#   glm  : third_party/include/glm     （header-only）
#   glfw : third_party/include/GLFW + third_party/lib/glfw3dll.lib + third_party/bin/glfw3.dll
#   Vulkan: LunarG SDK（VULKAN_SDK=E:\VULKAN\1.4，含头文件、vulkan-1.lib、校验层、glslangValidator）
#   shader 编译: 用 SDK 的 glslangValidator.exe（见 shaders/compile.bat）
#
# 说明：最初尝试 vcpkg manifest 模式，但本机 vcpkg 拉取 GitHub registry 失败（网络），
#       故改用静态 third_party + SDK 方案，换机/重装环境只需复制目录即可。
project(minecraft_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# 引擎源码：四轨模块（core/shared/business/physics/input/render/demo）递归收集
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
file(GLOB_RECURSE ENGINE_HEADERS CONFIGURE_DEPENDS src/*.h src/*.hpp)

add_executable(minecraft_engine
    ${ENGINE_SOURCES}
    ${ENGINE_HEADERS}
)

# ---- 第三方依赖 ----
set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# glm（header-only）+ glfw 头文件
target_include_directories(minecraft_engine PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${THIRD_PARTY_DIR}/include"
)

# Vulkan SDK（FindVulkan 通过 VULKAN_SDK 环境变量定位）
find_package(Vulkan REQUIRED)

target_link_libraries(minecraft_engine PRIVATE
    Vulkan::Vulkan
    "${THIRD_PARTY_DIR}/lib/glfw3dll.lib"   # GLFW 动态库导入库
)

# glfw3.dll 运行时依赖：构建后拷贝到 exe 目录
add_custom_command(TARGET minecraft_engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${THIRD_PARTY_DIR}/bin/glfw3.dll" "$<TARGET_FILE_DIR:minecraft_engine>/glfw3.dll"
)

# 源码用 UTF-8（含中文注释），MSVC 统一按 UTF-8 解析，避免 C4819 乱码警告
if(MSVC)
    target_compile_options(minecraft_engine PRIVATE /utf-8)
endif()

set_target_properties(minecraft_engine PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

# ---- 双保险：构建后把 shaders/*.spv 复制到 exe 所在目录的 shaders/ 子目录 ----
# 无论进程工作目录是工程根（VS 调试器）还是 exe 目录（命令行），
# 相对路径 shaders/vert.spv 都能被找到。
add_custom_command(TARGET minecraft_engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:minecraft_engine>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vert.spv" "$<TARGET_FILE_DIR:minecraft_engine>/shaders/vert.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/frag.spv" "$<TARGET_FILE_DIR:minecraft_engine>/shaders/frag.spv"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:minecraft_engine>/textures"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/textures/grass_side.png" "$<TARGET_FILE_DIR:minecraft_engine>/textures/grass_side.png"
)
