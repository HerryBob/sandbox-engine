#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace engine::save {

// ============================================================================
// SaveMenu：启动时的存档选择（原型，用 HUD 的 ASCII 文字，不做图形界面）
// ============================================================================
//
// 交互（全部键盘，键位与游戏内不冲突）：
//   UP / DOWN   选择
//   ENTER       进入选中存档
//   N           新建存档（名字用时间戳）
//   X           删除选中存档（再按一次 X 确认）
//   ESC         退出程序
//
// 为什么放在装配期（世界还没建）：换存档 = 换世界，运行期换世界要拆掉并重建
// 世界/生成器/物理/网格，那是另一个量级的改动（B137 记的就是这个缺口）。
// 装配期选好，后面的整条链按选中的世界名装配，零额外成本。
//
// 输出只有两样：`lines()` 给 HUD 画、`takeResult()` 给 main 决定下一步。
class SaveMenu {
public:
    enum class Result { None, Chosen, Quit };

    // root：存档根目录（<repo>/saves）。构造时扫描一次现有存档。
    explicit SaveMenu(std::filesystem::path root);

    // 重新扫描（新建/删除后调用）
    void refresh();

    const std::vector<std::string>& entries() const noexcept { return entries_; }
    const std::string& selectedName() const noexcept;
    bool empty() const noexcept { return entries_.empty(); }

    // 给 HUD 的行（ASCII；字体只覆盖 32..126）
    std::vector<std::string> lines() const;

    // 处理一个按键（**已做边沿检测**的按键码由调用方保证）；
    // 返回 Chosen 时 selectedName() 就是用户选中的存档名。
    Result onKey(int glfwKey);

    // 新建存档：返回真实生成的目录名（失败返回空串）
    std::string createNewSave();

private:
    std::filesystem::path root_;
    std::vector<std::string> entries_;
    std::size_t cursor_ = 0;
    bool deleteArmed_ = false;      // 按过一次 X，等待确认
    std::string deleteTarget_;
    std::string status_;            // 一行提示（删除确认/新建成功等）
};

} // namespace engine::save
