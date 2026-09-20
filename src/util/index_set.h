#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace xorcle {

/// @brief A bit-packed set of indices drawn from `[0, size())`, whose index
/// domain -- columns, matrix rows or variables -- is documented by each holder.
class IndexSet {
public:
    explicit IndexSet(std::size_t size = 0) : size_(size), words_(word_count(size), 0) {}

    std::size_t size() const noexcept { return size_; }

    /// @brief Remove every index, leaving the size unchanged.
    void clear() noexcept {
        for (std::uint64_t& word : words_) {
            word = 0;
        }
    }

    /// @brief Draw from `[0, size)` instead, removing every index and keeping
    /// whatever storage is already held.
    void resize(std::size_t size) {
        size_ = size;
        words_.assign(word_count(size), 0);
    }

    /// @brief Add every index of `other`.
    /// @pre `other.size() == size()`.
    void or_with(const IndexSet& other) noexcept {
        assert(other.size_ == size_ && "IndexSet::or_with size mismatch");
        for (std::size_t w = 0; w < words_.size(); ++w) {
            words_[w] |= other.words_[w];
        }
    }

    /// @pre `i < size()`, here and in `set()` and `reset()`.
    bool test(std::size_t i) const noexcept {
        assert(i < size_ && "IndexSet::test index is out of range");
        return (words_[i >> 6] >> (i & 63)) & 1u;
    }

    void set(std::size_t i) noexcept {
        assert(i < size_ && "IndexSet::set index is out of range");
        words_[i >> 6] |= std::uint64_t{1} << (i & 63);
    }

    void reset(std::size_t i) noexcept {
        assert(i < size_ && "IndexSet::reset index is out of range");
        words_[i >> 6] &= ~(std::uint64_t{1} << (i & 63));
    }

    /// @brief Number of set indices strictly below `i`.
    /// @pre `i <= size()`.
    std::size_t count_below(std::size_t i) const noexcept;

    std::size_t count() const noexcept;

    bool none() const noexcept;

    /// @brief Call `f(index)` for each set index, in ascending order.
    template <class F>
    void for_each_set(F&& f) const {
        for (std::size_t w = 0; w < words_.size(); ++w) {
            std::uint64_t word = words_[w];
            while (word != 0) {
                const std::size_t offset = static_cast<std::size_t>(std::countr_zero(word));
                f((w << 6) + offset);
                word &= word - 1;
            }
        }
    }

    bool operator==(const IndexSet& other) const noexcept = default;

private:
    static std::size_t word_count(std::size_t size) noexcept { return (size + 63) / 64; }

    std::size_t size_;
    std::vector<std::uint64_t> words_;
};

} // namespace xorcle
