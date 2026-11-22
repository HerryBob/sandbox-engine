#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace engine::registry {

    // 通用注册表：key → value，附"来源"标记（哪个模组注册的）。
    //
    // 线程模型（B74 起明确）：
    //   · **结构修改**（add / erase / eraseByOwner / freeze）互斥保护 —— 见 mutex_。
    //   · **读取**（find / countByOwner / ownerOf / size / begin / end）**不加锁**：
    //     find 返回的是内部节点指针，加锁也保不住它的生命周期，所以读侧的正确性
    //     依赖"没有并发结构修改"这个前提 —— 实际用法是：
    //       装配期（单线程）：只有 add；
    //       freeze 之后：只剩 erase/eraseByOwner（卸载路径），且与读取错峰。
    //     若将来要支持"运行期边改边读"，必须改成快照式接口（拷贝一份再读），
    //     而不是在这里加锁。
    template <typename T, typename Key = std::uint16_t>
    class Registry {
    public:
        // owner：注册来源（模组名；nullptr/空串 = 引擎自身注册）。
        bool add(Key key, const T& value, const char* owner = nullptr) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (frozen_) return false;
            // B75：两表必须同进同退。原来先 items_ 后 owners_ —— 若 owners_.emplace
            // 抛 bad_alloc，items_ 里就留了一个没有 owner 的条目（卸载时清不掉）。
            // 现在先插 owners_，items_ 失败/抛异常时回滚 owners_。
            owners_.emplace(key, (owner != nullptr) ? std::string(owner) : std::string());
            try {
                const auto result = items_.emplace(key, value);
                if (!result.second) {
                    owners_.erase(key);   // key 已被占用：回滚
                    return false;
                }
            } catch (...) {
                owners_.erase(key);
                throw;
            }
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
            std::lock_guard<std::mutex> lock(mutex_);
            owners_.erase(key);
            return items_.erase(key) > 0U;
        }

        // 按来源批量移除，返回移除条数（卸载闭环用）。
        std::size_t eraseByOwner(const char* owner) {
            std::lock_guard<std::mutex> lock(mutex_);
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
        void freeze() {
            std::lock_guard<std::mutex> lock(mutex_);
            frozen_ = true;
        }
        bool frozen() const { return frozen_; }
        std::size_t size() const { return items_.size(); }

        using const_iterator = typename std::unordered_map<Key, T>::const_iterator;
        const_iterator begin() const { return items_.begin(); }
        const_iterator end() const { return items_.end(); }

    private:
        // 只保护结构修改（add / erase / eraseByOwner / freeze）；读取不加锁，
        // 契约见类头注释。加 mutex 后 Registry 不可拷贝/移动 —— 实例都是
        // 进程级 inline 全局变量，本来也不该被拷贝。
        mutable std::mutex mutex_;
        std::unordered_map<Key, T> items_;
        std::unordered_map<Key, std::string> owners_;   // 与 items_ 同 key，保证一一对应
        bool frozen_ = false;
    };
}
