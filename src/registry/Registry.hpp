#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

namespace engine::registry {

    // 通用注册表：key → value，附"来源"标记（哪个模组注册的）。
    // 设计要点：
    //   ① add 只在封表前允许；erase / eraseByOwner 封表后仍允许
    //      （卸载发生在启动封表之后，"禁止新增"和"允许移除"是两件事）。
    //   ② 来源只存字符串副本，绝不留 DLL 里的指针 —— 卸载后 owner 名字仍可安全比对。
    template <typename T, typename Key = std::uint16_t>
    class Registry {
    public:
        // owner：注册来源（模组名；nullptr/空串 = 引擎自身注册）。
        bool add(Key key, const T& value, const char* owner = nullptr) {
            if (frozen_) return false;
            const auto result = items_.emplace(key, value);
            if (!result.second) return false;
            owners_.emplace(key, (owner != nullptr) ? std::string(owner) : std::string());
            return true;
        }

        const T* find(Key key) const {
            const auto it = items_.find(key);
            return (it == items_.end()) ? nullptr : &it->second;
        }

        // 刻意不提供 get()（引用返回）。历史版本是 assert(p) + return *p：
        // Release 下 assert 被编译掉，未注册 key 直接空指针解引用，崩溃且无信息。
        // 全工程调用方一律用 find() + 判空，语义更明确，故直接删除该接口。
        bool erase(Key key) {
            owners_.erase(key);
            return items_.erase(key) > 0U;
        }

        // 按来源批量移除，返回移除条数（卸载闭环用）。
        std::size_t eraseByOwner(const char* owner) {
            const std::string name = (owner != nullptr) ? std::string(owner) : std::string();
            std::size_t removed = 0;
            for (auto it = owners_.begin(); it != owners_.end();) {
                if (it->second == name) {
                    items_.erase(it->first);
                    it = owners_.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
            return removed;
        }

        // 按来源计数（卸载前后自检用）。
        std::size_t countByOwner(const char* owner) const {
            const std::string name = (owner != nullptr) ? std::string(owner) : std::string();
            std::size_t count = 0;
            for (const auto& entry : owners_) {
                if (entry.second == name) ++count;
            }
            return count;
        }

        // 查来源（诊断用；未注册 key 返回空串）。
        std::string ownerOf(Key key) const {
            const auto it = owners_.find(key);
            return (it == owners_.end()) ? std::string() : it->second;
        }

        // 封表：之后只允许读与移除，禁止新增（免锁读的前提）。
        void freeze() { frozen_ = true; }
        bool frozen() const { return frozen_; }
        std::size_t size() const { return items_.size(); }

        using const_iterator = typename std::unordered_map<Key, T>::const_iterator;
        const_iterator begin() const { return items_.begin(); }
        const_iterator end() const { return items_.end(); }

    private:
        std::unordered_map<Key, T> items_;
        std::unordered_map<Key, std::string> owners_;   // 与 items_ 同 key，保证一一对应
        bool frozen_ = false;
    };
}
