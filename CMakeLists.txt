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

# 显式链接，未来引入 Vulkan 渲染时在此追加
target_link_libraries(sandbox_engine PRIVATE)

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
