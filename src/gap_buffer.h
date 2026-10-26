#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace simple_notepad
{
    // a hole at the last edit means nearby edits move little; a Delta shifts everything after it in one add
    template <class Container, class Delta = std::monostate> class gap_buffer
    {
      public:
        using value_type = Container::value_type;

        gap_buffer() = default;
        explicit gap_buffer(Container items) : items_(std::move(items)), gap_(items_.size()), gap_end_(items_.size())
        {
        }

        [[nodiscard]] size_t size() const
        {
            return items_.size() - (gap_end_ - gap_);
        }

        [[nodiscard]] value_type operator[](size_t i) const
        {
            return i < gap_ ? items_[i] : shifted(items_[i + (gap_end_ - gap_)], tail_);
        }

        [[nodiscard]] value_type back() const
        {
            return (*this)[size() - 1];
        }

        // first index in [from, size) where pred turns false; pred must be true up to some point, then false
        template <class Pred> [[nodiscard]] size_t partition_point(Pred pred, size_t from = 0) const
        {
            size_t lo = from;
            size_t hi = size();
            while (lo < hi) // raw loop: a binary search over indices, since the hole breaks iterators
            {
                const size_t mid = lo + ((hi - lo) / 2);
                if (pred((*this)[mid]))
                {
                    lo = mid + 1;
                }
                else
                {
                    hi = mid;
                }
            }
            return lo;
        }

        // p[i] is element i over [from, to); a range already whole moves nothing, so old pointers stay valid
        [[nodiscard]] const value_type* contiguous(size_t from, size_t to)
            requires std::is_same_v<Delta, std::monostate>
        {
            if (gap_ > from && gap_ < to)
            {
                move_gap(to - gap_ < gap_ - from ? to : from);
            }
            return items_.data() + (gap_ >= to ? 0 : gap_end_ - gap_);
        }

        [[nodiscard]] std::basic_string_view<value_type> view(size_t from, size_t to)
            requires std::is_same_v<Delta, std::monostate>
        {
            return {contiguous(from, to) + from, to - from};
        }

        // the elements before and after the hole, in order, without moving anything
        [[nodiscard]] std::array<std::basic_string_view<value_type>, 2> runs() const
            requires std::is_same_v<Delta, std::monostate>
        {
            return {{{items_.data(), gap_}, {items_.data() + gap_end_, items_.size() - gap_end_}}};
        }

        // leaves the hole right after the inserted elements, so shift_tail() then moves everything after them
        void replace(size_t at, size_t remove, std::span<const value_type> insert)
        {
            move_gap(at);
            gap_end_ += remove; // the removed elements join the hole
            if (gap_end_ - gap_ < insert.size())
            {
                // grows by an eighth of the size so a long run of inserts reallocates rarely
                const size_t grow = insert.size() + (size() / 8) + 64;
                const size_t old_end = items_.size();
                items_.resize(old_end + grow);
                value_type* p = items_.data();
                std::move_backward(p + gap_end_, p + old_end, p + old_end + grow);
                gap_end_ += grow;
            }
            std::ranges::copy(insert, items_.data() + gap_);
            gap_ += insert.size();
        }

        void shift_tail(Delta d)
            requires(!std::is_same_v<Delta, std::monostate>)
        {
            tail_ = tail_ + d;
        }

      private:
        Container items_;
        size_t gap_ = 0;     // first slot of the hole
        size_t gap_end_ = 0; // first element after the hole
        Delta tail_{};

        static value_type shifted(value_type v, Delta d)
        {
            if constexpr (std::is_same_v<Delta, std::monostate>)
            {
                return v;
            }
            else
            {
                return v + d;
            }
        }

        void move_gap(size_t at)
        {
            value_type* p = items_.data();
            if (at < gap_)
            {
                const size_t n = gap_ - at;
                std::move_backward(p + at, p + gap_, p + gap_end_);
                gap_ = at;
                gap_end_ -= n;
                if constexpr (!std::is_same_v<Delta, std::monostate>)
                {
                    std::for_each(p + gap_end_, p + gap_end_ + n, [&](value_type& v) { v = v + -tail_; });
                }
            }
            else if (at > gap_)
            {
                const size_t n = at - gap_;
                if constexpr (!std::is_same_v<Delta, std::monostate>)
                {
                    std::for_each(p + gap_end_, p + gap_end_ + n, [&](value_type& v) { v = v + tail_; });
                }
                std::move(p + gap_end_, p + gap_end_ + n, p + gap_);
                gap_ = at;
                gap_end_ += n;
            }
        }
    };
} // namespace simple_notepad
