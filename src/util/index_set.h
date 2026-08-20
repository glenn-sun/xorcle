#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace xorcle {

/// @brief A bit-packed set of indices drawn from `[0, size())`. The index domain
/// is whatever the holder says it is -- columns, matrix rows or variables -- so
/// each use site documents its own. A plain concrete container, not part of the
/// algebra dispatch surface (see `algebra/equation.h`): per-bit access on it is a
/// direct inlined call and carries none of the cost that motivates the coarse
/// algebra interface.
///
/// @note Bit packing is not an optimization here but a requirement. Every
/// `Clause` carries a pivot column set of `num_vars + 1` indices, so a
/// byte-per-index representation would cost eight times as much across the
/// clause database.
class IndexSet {
public:
    explicit IndexSet(std::size_t size = 0) : size_(size), words_(word_count(size), 0) {}

    std::size_t size() const noexcept { return size_; }

    /// @brief Resize to a new index domain, zeroing the whole set.
    void reset_size(std::size_t size) {
        size_ = size;
        words_.assign(word_count(size), 0);
    }

    /// @brief Clear every bit, leaving the size unchanged.
    void clear() noexcept {
        for (std::uint64_t& word : words_) {
            word = 0;
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

    /// @brief Number of set indices strictly below `i`. This is how a row's
    /// insertion index into a row-echelon matrix is found: a row with leading
    /// column `c` is inserted at `count_below(c)`.
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
                const std::size_t bit = static_cast<std::size_t>(std::countr_zero(word));
                f((w << 6) + bit);
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
