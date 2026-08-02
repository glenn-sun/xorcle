#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace xorcle {

/// @brief A fixed-width, bit-packed set of column indices.
///
/// Used for pivot-column bookkeeping and for reduction coefficient vectors.
/// This is a plain concrete container, not part of the algebra dispatch surface
/// (see `algebra/equation.h`): per-bit access on it is a direct inlined call and
/// carries none of the cost that motivates the coarse algebra interface.
///
/// @note Bit packing is not an optimization here but a requirement. Every
/// `Clause` carries a pivot bitmap of width `num_vars + 1`, so a byte-per-column
/// representation would cost eight times as much across the clause database.
class Bitmap {
public:
    /// @brief Construct an all-zero bitmap of the given width.
    explicit Bitmap(std::size_t width = 0) : width_(width), words_(word_count(width), 0) {}

    /// @brief Number of columns this bitmap covers.
    std::size_t size() const noexcept { return width_; }

    /// @brief Resize to a new width, zeroing the whole bitmap.
    void reset_width(std::size_t width) {
        width_ = width;
        words_.assign(word_count(width), 0);
    }

    /// @brief Clear every bit, leaving the width unchanged.
    void clear() noexcept {
        for (std::uint64_t& word : words_) {
            word = 0;
        }
    }

    /// @brief Return whether column `i` is set.
    /// @pre `i < size()`.
    bool test(std::size_t i) const noexcept {
        return (words_[i >> 6] >> (i & 63)) & 1u;
    }

    /// @brief Set column `i`.
    /// @pre `i < size()`.
    void set(std::size_t i) noexcept { words_[i >> 6] |= std::uint64_t{1} << (i & 63); }

    /// @brief Clear column `i`.
    /// @pre `i < size()`.
    void reset(std::size_t i) noexcept { words_[i >> 6] &= ~(std::uint64_t{1} << (i & 63)); }

    /// @brief Number of set columns strictly below `i`.
    ///
    /// This is how a row's insertion index into a row-echelon matrix is found:
    /// a row with leading column `c` is inserted at `count_below(c)`.
    /// @pre `i <= size()`.
    std::size_t count_below(std::size_t i) const noexcept;

    /// @brief Total number of set columns.
    std::size_t count() const noexcept;

    /// @brief Whether no column is set.
    bool none() const noexcept;

    /// @brief Call `f(column)` for each set column, in ascending order.
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

    /// @brief Compare two bitmaps of equal width.
    bool operator==(const Bitmap& other) const noexcept = default;

private:
    static std::size_t word_count(std::size_t width) noexcept { return (width + 63) / 64; }

    std::size_t width_;
    std::vector<std::uint64_t> words_;
};

} // namespace xorcle
