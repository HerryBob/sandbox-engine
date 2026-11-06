#pragma once

#include <unordered_map>
#include <cstdint>
#include <cstddef>

namespace engine::registry {
    template <typename T, typename Key = std::uint8_t>
    class Registry {
    public:
        bool add(Key key, const T& value) {
            if (frozen_) return false;
            return items_.emplace(key, value).second;
        }

        const T* find(Key key) const {
            auto it = items_.find(key);
            return it == items_.end() ? nullptr : &it->second;
        }

        // 刻意不提供 get()（引用返回）。历史版本是 assert(p) + return *p：
        // Release 下 assert 被编译掉，未注册 key 直接空指针解引用，崩溃且无信息。
        // 全工程调用方一律用 find() + 判空，语义更明确，故直接删除该接口。
        void freeze() { frozen_ = true; }
        bool frozen() const { return frozen_; }
        std::size_t size() const { return items_.size(); }

        using const_iterator = typename std::unordered_map<Key, T>::const_iterator;
        const_iterator begin() const { return items_.begin(); }
        const_iterator end() const { return items_.end(); }


    private:
        std::unordered_map<Key, T> items_;
        bool frozen_ = false;
    };
}