#pragma once

#include <unordered_map>
#include <cstdint>
#include <cassert>
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

        const T& get(Key key) const {
            const T* p = find(key);
            assert(p && "get: Key未注册");
            return *p;
        }
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