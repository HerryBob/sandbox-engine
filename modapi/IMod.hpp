#pragma once

#include "modapi/api_base.h"
#include "modapi/block_api.h"
#include "modapi/noise_api.h"

namespace modapi {

// 引擎注入给模组的全量能力包：按需取用子结构。
// 需要方块 → api->block.*；需要噪声 → api->noise.*；日志/版本是基类字段，两边都有。
// 扩展纪律：新增能力 = 新子结构，追加在本结构末尾 ——
// 老模组访问 block/noise 的偏移不变，不重编也能加载（同 v1→v2 的 ABI 纪律）。
struct ModApi {
    BlockApi block;   // 方块注册能力
    NoiseApi noise;   // 噪声注册能力
};

// 模组入口：引擎通过 CreateMod 得到它，再按五阶段生命周期驱动。
// 跨 DLL 虚接口依赖同编译器同虚表布局；虚函数顺序发布后不可改（扩展走 IModV2）。
class IMod {
public:
    virtual ~IMod() {}
    virtual const char* name() const = 0;
    virtual const char* version() const = 0;
    virtual bool onLoad(const ModApi* api) = 0;
    virtual void onUnload() = 0;
};

using CreateModFn  = IMod* (*)(const ModApi* api);
using DestroyModFn = void  (*)(IMod* mod);  

inline constexpr const char* kCreateModSymbol  = "CreateMod";
inline constexpr const char* kDestroyModSymbol = "DestroyMod";

} // namespace modapi
