#include "save/SaveMenu.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

#include <GLFW/glfw3.h>

#include "core/Log.hpp"

namespace engine::save {

namespace fs = std::filesystem;

namespace {

// 存档目录判定：有 level.json 或 chunks/ 才算（防止把别的东西当存档）
bool looksLikeSave(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    return fs::exists(dir / "level.json", ec) || fs::is_directory(dir / "chunks", ec);
}

// 时间戳名字：save-20261126-201500（本地时间，按名字排序即按时间排序）
std::string timestampName() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "save-%04d%02d%02d-%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

} // namespace

SaveMenu::SaveMenu(fs::path root) : root_(std::move(root)) { refresh(); }

void SaveMenu::refresh() {
    entries_.clear();
    std::error_code ec;
    fs::create_directories(root_, ec);
    for (const auto& entry : fs::directory_iterator(root_, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;
        if (!looksLikeSave(entry.path())) continue;
        entries_.push_back(entry.path().filename().string());
    }
    // 新的在前（时间戳命名 → 字典序即时间序）
    std::sort(entries_.begin(), entries_.end(), std::greater<>());
    if (cursor_ >= entries_.size()) cursor_ = entries_.empty() ? 0 : entries_.size() - 1;
}

const std::string& SaveMenu::selectedName() const noexcept {
    static const std::string kEmpty;
    if (entries_.empty()) return kEmpty;
    return entries_[cursor_];
}

std::vector<std::string> SaveMenu::lines() const {
    std::vector<std::string> out;
    out.push_back("[SAVE] SELECT A WORLD");
    out.push_back("");
    if (entries_.empty()) {
        out.push_back("  (no saves yet - press N to create one)");
        out.push_back("");
    } else {
        // 只列前 12 个：字体行高有限，菜单不做滚动页（原型）
        const std::size_t shown = std::min<std::size_t>(entries_.size(), 12);
        for (std::size_t i = 0; i < shown; ++i) {
            const bool sel = (i == cursor_);
            out.push_back(std::string(sel ? "> " : "  ") + entries_[i] +
                          (sel ? "   <--" : ""));
        }
        if (entries_.size() > shown) out.push_back("  ...");
        out.push_back("");
    }
    out.push_back("UP/DOWN  ENTER=load  N=new  X=delete  ESC=quit");
    if (!status_.empty()) out.push_back(status_);
    return out;
}

SaveMenu::Result SaveMenu::onKey(int key) {
    if (key == GLFW_KEY_UP && !entries_.empty()) {
        deleteArmed_ = false;
        cursor_ = (cursor_ == 0) ? entries_.size() - 1 : cursor_ - 1;
        return Result::None;
    }
    if (key == GLFW_KEY_DOWN && !entries_.empty()) {
        deleteArmed_ = false;
        cursor_ = (cursor_ + 1) % entries_.size();
        return Result::None;
    }
    if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) {
        if (entries_.empty()) {
            status_ = "(nothing selected - press N)";
            return Result::None;
        }
        return Result::Chosen;
    }
    if (key == GLFW_KEY_N) {
        const std::string name = createNewSave();
        if (name.empty()) {
            status_ = "(create failed)";
        } else {
            refresh();
            for (std::size_t i = 0; i < entries_.size(); ++i)
                if (entries_[i] == name) cursor_ = i;
            status_ = "(created " + name + ")";
        }
        deleteArmed_ = false;
        return Result::None;
    }
    if (key == GLFW_KEY_X) {
        if (entries_.empty()) return Result::None;
        if (!deleteArmed_ || deleteTarget_ != selectedName()) {
            deleteArmed_ = true;
            deleteTarget_ = selectedName();
            status_ = "(press X again to DELETE " + deleteTarget_ + ")";
            return Result::None;
        }
        // 二次确认 → 删除
        std::error_code ec;
        const auto dir = root_ / deleteTarget_;
        fs::remove_all(dir, ec);
        if (ec) {
            status_ = "(delete failed)";
        } else {
            status_ = "(deleted " + deleteTarget_ + ")";
            refresh();
        }
        deleteArmed_ = false;
        deleteTarget_.clear();
        return Result::None;
    }
    if (key == GLFW_KEY_ESCAPE) return Result::Quit;
    return Result::None;
}

std::string SaveMenu::createNewSave() {
    // 时间戳可能撞（同一秒内连按）：加后缀
    std::string name = timestampName();
    std::error_code ec;
    int suffix = 1;
    while (fs::exists(root_ / name, ec)) {
        name = timestampName() + "-" + std::to_string(suffix++);
        if (suffix > 99) return {};
    }
    const fs::path dir = root_ / name;
    fs::create_directories(dir / "chunks", ec);
    if (ec) {
        ENGINE_LOG_ERROR("Save", "新建存档失败：" + dir.string() + "（" + ec.message() + "）");
        return {};
    }
    return name;
}

} // namespace engine::save
