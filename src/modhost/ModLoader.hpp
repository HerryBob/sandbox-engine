#pragma once
// ============================================================================
// src/modhost/ModLoader.hpp —— DLL 模组加载器（host 侧）
// ============================================================================
// 职责范围（六阶段管线中的前四步）：
//   ① 发现：扫描模组目录下的 *.dll
//   ② 校验：检查导出符号 CreateMod / DestroyMod 是否齐全
//   ③ 加载：LoadLibraryW 载入 DLL，GetProcAddress 取函数指针
//   ④ 初始化：调用 CreateMod(&api) 拿到 IMod*，再调 onLoad 注册内容
//
// 不在本文件：⑤ 调度（事件总线，后续阶段）⑥ 卸载（本类提供 unloadAll，
// 但"谁创建谁销毁"的顺序纪律在阶段 5 才完整落地）。
//
// 目录不存在 / 目录为空都是正常情况（0 个模组）——引擎必须能在无模组时独立运行，
// 这是"生成器接口留在引擎、实现搬进 DLL"这条边界的自举前提。
// ============================================================================

#include "modapi/IMod.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace engine::modhost {

class ModLoader {
public:
    // 扫描目录下所有 *.dll，逐个校验并加载。返回成功加载的模组数量。
    // 单个模组失败不影响其它模组（失败会打日志并继续下一个）。
    static std::size_t loadAll(const std::string& dir);

    // 逆序卸载：先 onUnload，再 DestroyMod，最后 FreeLibrary。
    // 逆序是因为后面加载的模组可能依赖前面模组注册的内容。
    static void unloadAll();

    // 当前已加载的模组数量。
    static std::size_t loadedCount();

private:
    // 一条已加载模组的记录。handle 用 void* 存储，避免在头文件里引入 Windows.h
    // （Windows.h 的 min/max 宏会污染整个包含链）。
    struct LoadedMod {
        std::string           path;      // 完整路径（日志用）
        void*                 handle;    // HMODULE
        modapi::IMod*         instance;  // 模组实例（归属 DLL）
        modapi::DestroyModFn  destroy;   // 模组导出的销毁函数
    };

    static std::vector<LoadedMod> s_mods_;
};

} // namespace engine::modhost
