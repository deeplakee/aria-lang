#ifndef ARIA_ARIA_ARRAY_HPP
#define ARIA_ARIA_ARRAY_HPP

#include "common.hpp"
#include "memory/Array.hpp"
#include "memory/GC.hpp"
#include "value/Value.hpp"

namespace aria {

    class AriaArray : public Array<Value> {
    public:
        using Array<Value>::Array;

        void trace(GC& gc) const noexcept {
            for (const Value& v: *this) {
                gc.mark_value(v);
            }
        }

        [[nodiscard]]
        Opt<usize> find(const Value& target) const noexcept {
            for (usize index = 0; index < size(); ++index) {
                if (value_equal(data()[index], target)) {
                    return index;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]]
        bool contains(const Value& target) const noexcept {
            return find(target).has_value();
        }

        // 保序移除所有 == 命中元素，返回是否命中。
        bool remove(const Value& target) noexcept {
            const usize old_size = size();
            usize       kept     = 0;
            for (usize index = 0; index < old_size; ++index) {
                if (!value_equal(data()[index], target)) {
                    data()[kept] = data()[index];
                    ++kept;
                }
            }
            truncate(kept);
            return kept != old_size;
        }
    };

} // namespace aria

#endif // ARIA_ARIA_ARRAY_HPP
