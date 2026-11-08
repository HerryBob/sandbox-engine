#pragma once

#include "modapi/noise_api.h"
#include "worldgen/noise/NoiseHandle.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::worldgen {

// 噪声 provider 注册表：type 名 ↔ 函数指针三元组（模组口子）。
// 与 registry/Registry<T> 模板的区别：登记的不是 C++ 对象，而是一组
// 跨边界函数指针（POD）—— 这样 DLL 与 exe 之间不需要共享任何 ABI。
// 生命周期纪律对齐 Registry<T>：启动期注册（模组加载 + 引擎内置）→
// freeze() 锁表 → 之后只读查表。
class NoiseRegistry {
public:
    // 注册一个 provider（模组 DLL 与引擎内置走同一入口、地位平等）。
    // type 空 / create·sample·destroy 任一为空 / 重复 / 已冻结 → 拒绝。
    // owner：注册来源（模组名；nullptr/空串 = 引擎自身注册），只存字符串副本。
    static bool registerType(const modapi::NoiseProviderDef& def, const char* owner = nullptr);

    // 按来源批量移除（卸载闭环）：表里存的是 DLL 的 create/sample/destroy 函数指针，
    // DLL 一旦 FreeLibrary，这些指针就是悬垂指针 —— 卸载方必须调本函数清干净。
    // 封表后仍允许（移除比新增安全）。返回移除条数。
    static std::size_t eraseByOwner(const char* owner);

    // 按来源计数（卸载前后自检用）。
    static std::size_t countByOwner(const char* owner);

    // 按 type 创建句柄；params 只在本次调用期间有效（DLL 负责拷贝自己需要的值）。
    // 未注册 / create 返回空 → false（stderr 说明原因），out 已被 reset。
    static bool make(const char* type,
                     const modapi::NoiseParamC* params, std::int32_t count,
                     NoiseHandle& out);

    // 是否已注册（PresetLoader 早期校验用：先把"type 拼错"挡在加载阶段）。
    static bool contains(const char* type);

    // 已注册数量 / 名称列表（日志与自检用）。
    static std::size_t size();
    static std::vector<std::string> types();

    // 锁表：启动期注册完成后调用，之后只读（免锁读的前提）。
    static void freeze();
    static bool frozen();

private:
    // 函数局部静态（Meyer's singleton）：跨 TU 安全的单实例，首次访问时构造。
    static std::unordered_map<std::string, modapi::NoiseProviderDef>& table();
    // 与 table() 同 key 的来源标记（同样是字符串副本）。
    static std::unordered_map<std::string, std::string>& owners();
    static bool& frozenFlag();
};

} // namespace engine::worldgen
