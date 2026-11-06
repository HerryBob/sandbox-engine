#pragma once

#include "modapi/IMod.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace engine::modhost {

class ModLoader {
public:
    static std::size_t loadAll(const std::string& dir);

    static void unloadAll();

    static std::size_t loadedCount();

private:

    struct LoadedMod {
        std::string           path;      // 完整路径（日志用）
        void*                 handle;    // HMODULE
        modapi::IMod*         instance;  // 模组实例（归属 DLL）
        modapi::DestroyModFn  destroy;   // 模组导出的销毁函数
    };

    static std::vector<LoadedMod> s_mods_;
};

} // namespace engine::modhost
