#pragma once

#include "modapi/IMod.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace engine::business { class World; }

namespace engine::modhost {

class ModLoader {
public:
    static std::size_t loadAll(const std::string& dir);

    static void unloadAll();

    static std::size_t loadedCount();

    // 把 World 交给 API 层。World 在模组 onLoad 之后才构造，
    // 未 attach 期间所有世界查询返回 kWorldNotReady。
    static void attachWorld(engine::business::World* world);

private:

    struct LoadedMod {
        std::string           path;      // 完整路径（日志用）
        std::string           name;      // 模组名副本（注册来源标记用，绝不留 DLL 里的指针）
        void*                 handle;    // HMODULE
        modapi::IMod*         instance;  // 模组实例（归属 DLL）
        modapi::DestroyModFn  destroy;   // 模组导出的销毁函数
    };

    static std::vector<LoadedMod> s_mods_;
};

} // namespace engine::modhost
