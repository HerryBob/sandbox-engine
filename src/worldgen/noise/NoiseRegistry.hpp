#pragma once

#include "worldgen/noise/NoiseSource.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace engine::worldgen {

// 噪声类型注册表：type 名 ↔ 工厂函数（模组口子）。
// 与 registry/Registry<T> 模板的区别：NoiseSource 是抽象基类，需 unique_ptr 多态，
// 值语义模板塞不进去 —— 领域注册表随领域走（registry/ 保持纯通用模板）。
// 生命周期纪律对齐 Registry<T>：启动期注册 → freeze() 锁表 → 之后只读查表。
class NoiseRegistry {
public:
    using NoiseFactory = std::function<std::unique_ptr<NoiseSource>(const NoiseParams&)>;

    // 注册一种噪声类型（模组/DLL 口子）。type 空 / 工厂为空 / 重复 / 已冻结 → 拒绝。
    static bool registerType(std::string_view type, NoiseFactory factory);

    // 按 type 查表实例化；未注册 → nullptr + stderr 报错（不崩溃）。
    static std::unique_ptr<NoiseSource> make(std::string_view type, const NoiseParams& params);

    // 锁表：启动期注册完成后调用，之后只读（免锁读前提）。
    static void freeze();
    static bool frozen();

private:
    // 函数局部静态（Meyer's singleton）：跨 TU 安全的单实例，首次访问时构造。
    static std::unordered_map<std::string, NoiseFactory>& factories();
    static bool& frozenFlag();
};

} // namespace engine::worldgen
