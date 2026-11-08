cmake_minimum_required(VERSION 3.20)

# 体素沙盒引擎（Voxel Sandbox Engine）——参考《Vulkan编程指南》学习 Vulkan API，代码为自研重构
#
# 依赖策略（离线、可控，不依赖 vcpkg/网络）：
#   glm  : third_party/include/glm     （header-only）
#   glfw : third_party/include/GLFW + third_party/lib/glfw3dll.lib + third_party/bin/glfw3.dll
#   Vulkan: LunarG SDK（VULKAN_SDK=E:\VULKAN\1.4，含头文件、vulkan-1.lib、校验层、glslangValidator）
#   shader 编译: 构建时用 SDK 的 glslangValidator.exe 自动编译（见下方 compile_shaders 目标）
#
# 说明：最初尝试 vcpkg manifest 模式，但本机 vcpkg 拉取 GitHub registry 失败（网络），
#       故改用静态 third_party + SDK 方案，换机/重装环境只需复制目录即可。
project(sandbox_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# 引擎源码：四轨模块（core/shared/business/physics/input/render/demo）递归收集
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
file(GLOB_RECURSE ENGINE_HEADERS CONFIGURE_DEPENDS src/*.h src/*.hpp)

add_executable(sandbox_engine
    ${ENGINE_SOURCES}
    ${ENGINE_HEADERS}
)

# windows.h 会定义 min/max 宏，直接撞 rapidjson 里的 std::max(...)：
# 先被 include 的头文件（如 ModLoader.hpp）一旦拉进 windows.h，后包含 rapidjson 就编译失败。
# NOMINMAX 让 windows.h 不再定义这两个宏。
target_compile_definitions(sandbox_engine PRIVATE NOMINMAX)

# ---- 第三方依赖 ----
set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# glm（header-only）+ glfw 头文件 + 工程根（modapi/ 跨边界契约层）
target_include_directories(sandbox_engine PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${THIRD_PARTY_DIR}/include"
)

# Vulkan SDK（FindVulkan 通过 VULKAN_SDK 环境变量定位）
find_package(Vulkan REQUIRED)

# spdlog（日志库，编译库模式，MIT）：源码 vendored 到 third_party/spdlog，目标机现场编译。
# 只编库本体：关掉 example/tests/bench，避免拉 catch2 等额外依赖。
# 编译库宏 SPDLOG_COMPILED_LIB 由 spdlog::spdlog target 的 INTERFACE 自动传递，业务代码不需要手写。
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${THIRD_PARTY_DIR}/spdlog" EXCLUDE_FROM_ALL)

target_link_libraries(sandbox_engine PRIVATE
    Vulkan::Vulkan
    "${THIRD_PARTY_DIR}/lib/glfw3dll.lib"   # GLFW 动态库导入库
    spdlog::spdlog                          # 日志库（编译库模式；fmt 走 spdlog 内置 bundled 版本）
)

# glfw3.dll 运行时依赖：构建后拷贝到 exe 目录
add_custom_command(TARGET sandbox_engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${THIRD_PARTY_DIR}/bin/glfw3.dll" "$<TARGET_FILE_DIR:sandbox_engine>/glfw3.dll"
)

# 源码用 UTF-8（含中文注释），MSVC 统一按 UTF-8 解析，避免 C4819 乱码警告
if(MSVC)
    target_compile_options(sandbox_engine PRIVATE /utf-8)
endif()

set_target_properties(sandbox_engine PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

# ---- shader 编译：GLSL 源码（shaders/*.vert/.frag）→ SPIR-V（exe 同目录 shaders/*.spv）----
# 源码树只保存 GLSL 源码，不保存编译产物；构建时自动编译，改 shader 后重新构建即生效。
# 运行时工作目录由 main() 的 chdirToExeDir 固定到 exe 目录，相对路径 shaders/vert.spv 必定命中。
# 用 LunarG SDK 自带的 glslangValidator（FindVulkan REQUIRED 已依赖 VULKAN_SDK 环境变量）。
set(GLSLC "$ENV{VULKAN_SDK}/Bin/glslangValidator.exe")
if(NOT EXISTS "${GLSLC}")
    message(FATAL_ERROR "找不到 glslangValidator：${GLSLC}（请检查 VULKAN_SDK 环境变量）")
endif()

add_custom_target(compile_shaders ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sandbox_engine>/shaders"
    COMMAND "${GLSLC}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/block.vert"
            -o "$<TARGET_FILE_DIR:sandbox_engine>/shaders/vert.spv"
    COMMAND "${GLSLC}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/block.frag"
            -o "$<TARGET_FILE_DIR:sandbox_engine>/shaders/frag.spv"
    COMMAND "${GLSLC}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hud.vert"
            -o "$<TARGET_FILE_DIR:sandbox_engine>/shaders/hud_vert.spv"
    COMMAND "${GLSLC}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hud.frag"
            -o "$<TARGET_FILE_DIR:sandbox_engine>/shaders/hud_frag.spv"
    COMMENT "编译 shader：GLSL → SPIR-V（shaders/*.vert/.frag → shaders/*.spv）"
    VERBATIM
)
add_dependencies(sandbox_engine compile_shaders)
# 引擎自带资源（可选：公开仓库不含 MC 版权纹理，缺省不影响编译）
# assets/ 编排：assets/models/block/*.json + assets/textures/{block,font}/*.png
# data/ 编排：data/worldgen/presets/*.json
#
# 用 ALL 目标而不是 sandbox_engine 的 POST_BUILD —— POST_BUILD 只在 exe 重链时执行，
# 只改 JSON/贴图不动代码时资源不会同步到输出目录。
# 先 remove_directory 再 copy_directory：copy_directory 只增不删，删掉的资源会残留在
# 输出目录，索引照样能找到它，旧副本会把新来源（模组那份）遮蔽掉。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    add_custom_target(copy_engine_assets ALL
        COMMAND ${CMAKE_COMMAND} -E remove_directory
            "$<TARGET_FILE_DIR:sandbox_engine>/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:sandbox_engine>/assets"
    )
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
    add_custom_target(copy_engine_data ALL
        COMMAND ${CMAKE_COMMAND} -E remove_directory
            "$<TARGET_FILE_DIR:sandbox_engine>/data"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/data" "$<TARGET_FILE_DIR:sandbox_engine>/data"
    )
endif()

# ============================================================================
# 模组 DLL（#41 最小骨架）
# ============================================================================
# mods_src/ 下每个子目录 = 一个模组工程；产物拷到 exe 同级的 mods/<模组名>/（目录即模组单元）。
# MODULE 库在 Windows 上就是 .dll（PREFIX "" 去掉 lib 前缀）。
# 关键：DLL 只 include modapi/，绝不 include src/ —— 否则会拿到注册表的独立副本。
# 运行库必须与 exe 一致（Debug 下 MSVC 默认都是 /MDd；不要单独设 /MT）。
# builtin_blocks：引擎原来写死的那 6 个方块（#39）——删掉这个模组，方块注册表就是空的。
# 产物目录 mods/blocks/：内含 blocks.dll 与 mod.json（phase=builtin，保证先于其它模组加载）。
add_library(builtin_blocks MODULE
    mods_src/blocks/BuiltinBlocksMod.cpp
)
target_include_directories(builtin_blocks PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(builtin_blocks PRIVATE /utf-8)
endif()
set_target_properties(builtin_blocks PROPERTIES
    PREFIX ""
    OUTPUT_NAME "blocks"
    SUFFIX ".dll"
    FOLDER "mods"
)

# mod_noise：噪声算法实现（perlin / fbm）—— 从 src/worldgen/noise/ 搬出来的。
# 引擎侧只留注册表 + flat；这里编译的三个 .cpp 只依赖本目录头 + modapi/。
# （搬走后 src/ 的 GLOB 不再收录它们，引擎 exe 里自然没有这些算法。）
add_library(mod_noise MODULE
    mods_src/noise/NoiseMod.cpp
    mods_src/noise/PerlinNoise.cpp
    mods_src/noise/FbmNoise.cpp
)
target_include_directories(mod_noise PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/mods_src/noise"
)
if(MSVC)
    target_compile_options(mod_noise PRIVATE /utf-8)
endif()
set_target_properties(mod_noise PROPERTIES
    PREFIX ""
    SUFFIX ".dll"
    FOLDER "mods"
)

# 先建 DLL 再链接 exe（保证运行前 mods/ 已就绪）
add_dependencies(sandbox_engine builtin_blocks mod_noise)

# 拷贝挂在各自 DLL 的 POST_BUILD：DLL 重链即同步 mods/ 副本。
# （挂在 sandbox_engine 上会有漏洞：只改模组源码时 exe 不重链，POST_BUILD 不执行，
#   mods/ 里仍是旧 DLL。）
add_custom_command(TARGET builtin_blocks POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sandbox_engine>/mods/blocks"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:builtin_blocks>" "$<TARGET_FILE_DIR:sandbox_engine>/mods/blocks/blocks.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/mods_src/blocks/mod.json"
        "$<TARGET_FILE_DIR:sandbox_engine>/mods/blocks/mod.json"
)
# 模组自带资源：assets/（贴图、模型）与 data/（数据驱动定义）整体跟着模组走。
# 用 ALL 目标而不是 DLL 的 POST_BUILD（POST_BUILD 只在 DLL 重新链接时执行）；
# 先 remove_directory 再 copy_directory（copy_directory 只增不删，删掉的资源会残留）。
# EXISTS 在配置期求值：新增/删除某模组的 assets 或 data 目录后要重新配置一次。
foreach(mod_dir blocks noise)
    foreach(res_kind assets data)
        set(res_src "${CMAKE_CURRENT_SOURCE_DIR}/mods_src/${mod_dir}/${res_kind}")
        if(EXISTS "${res_src}")
            add_custom_target(copy_${mod_dir}_${res_kind} ALL
                COMMAND ${CMAKE_COMMAND} -E remove_directory
                    "$<TARGET_FILE_DIR:sandbox_engine>/mods/${mod_dir}/${res_kind}"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${res_src}"
                    "$<TARGET_FILE_DIR:sandbox_engine>/mods/${mod_dir}/${res_kind}"
            )
        endif()
    endforeach()
endforeach()
add_custom_command(TARGET mod_noise POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:sandbox_engine>/mods/noise"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:mod_noise>" "$<TARGET_FILE_DIR:sandbox_engine>/mods/noise/mod_noise.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/mods_src/noise/mod.json"
        "$<TARGET_FILE_DIR:sandbox_engine>/mods/noise/mod.json"
)
