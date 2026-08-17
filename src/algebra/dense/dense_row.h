#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <optional>
#include <span>

#include <gf2/gf2.h>

#include "algebra/algebra.h"
#include "algebra/reduce_result.h"

namespace xorcle {

/// @brief The bit-packed `Row` implementation, on `gf2::BitVector`.
///
/// This header and `dense_row.cpp` are the only files permitted to include gf2.
/// The word type is spelled
/// explicitly because `gf2::BitVector<>` defaults to `std::size_t`, which here
/// is `unsigned long` while `gf2::u64` is `unsigned long long`; gf2's binary
/// operators require identical word types, so mixing the two silently fails to
/// compile or compares unequal.
class DenseRow {
public:
    static constexpr Algebra algebra = Algebra::dense;

    using Storage = gf2::BitVector<gf2::u64>;
    using Word = Storage::word_type;

    /// @brief Construct the all-zero row of width `num_vars + 1`.
    explicit DenseRow(std::size_t width) : bits_(width) {}

    std::size_t width() const noexcept { return bits_.size(); }

    /// @brief Resize to `width` columns, zeroing any new ones.
    void resize(std::size_t width) { bits_.resize(width); }

    void clear() noexcept { gf2::set_all(bits_, false); }

    /// @brief Clear, then set each listed column; each must be below `width()`.
    void assign_columns(std::span<const std::size_t> columns) {
        clear();
        for (const std::size_t column : columns) {
            set(column);
        }
    }

    /// @pre `i < width()`.
    bool test(std::size_t i) const noexcept { return bits_.get(i); }

    /// @pre `i < width()`.
    void set(std::size_t i) noexcept { gf2::set(bits_, i, true); }

    /// @brief First set column, or `std::nullopt` if none is set. A word scan
    /// over `store()`, for the reason given on `xor_assign`.
    std::optional<std::size_t> leading_column() const noexcept {
        const Word* words = bits_.store();
        const std::size_t num_words = bits_.words();
        for (std::size_t w = 0; w < num_words; ++w) {
            if (words[w] != 0) {
                return (w << word_shift) + static_cast<std::size_t>(std::countr_zero(words[w]));
            }
        }
        return std::nullopt;
    }

    /// @brief First set column strictly after `i`, or `std::nullopt`.
    ///
    /// A word scan over `store()` rather than gf2's `next_set`. The two agree
    /// exactly, but gf2 masks the starting word inside its scan loop, via a
    /// `reset_bits` call guarded by an `i == word_index` test, and re-checks the
    /// word index against `words()` having already checked the bit index against
    /// `size()`. Written out, the mask is one branch-free shift and the loop
    /// carries no per-word conditional -- and this runs once per row operation
    /// in the reduction loop, where that is a measurable share of the iteration.
    /// @pre `i < width()`.
    std::optional<std::size_t> next_column(std::size_t i) const noexcept {
        const std::size_t next = i + 1;
        if (next >= bits_.size()) {
            return std::nullopt;
        }
        const Word* words = bits_.store();
        const std::size_t num_words = bits_.words();
        std::size_t w = next >> word_shift;
        Word word = words[w] & (~Word{0} << (next & word_mask));
        while (word == 0) {
            if (++w >= num_words) {
                return std::nullopt;
            }
            word = words[w];
        }
        return (w << word_shift) + static_cast<std::size_t>(std::countr_zero(word));
    }

    bool is_zero() const noexcept { return bits_.none(); }

    /// @brief Classify as @f$\mathbf{0}@f$, @f$\mathbf{1}@f$, or a leading
    /// column, in one word scan rather than separate emptiness and
    /// constant-column queries: a leading column at `width() - 1` can only mean
    /// the constant column alone is set.
    ReduceResult classify() const noexcept {
        const std::optional<std::size_t> leading = leading_column();
        if (!leading) {
            return ReduceResult::eq_true();
        }
        if (*leading + 1 == bits_.size()) {
            return ReduceResult::eq_false();
        }
        return ReduceResult::leading(*leading);
    }

    /// @brief Add another row into this one over GF(2).
    ///
    /// A word loop over `store()` rather than `bits_ ^= other.bits_`. gf2's
    /// `operator^=` writes each word through `set_word`, which re-`clean()`s the
    /// final word on every call -- a read-modify-write plus a `size() % 64`
    /// branch, taken essentially always for an @f$n+1@f$-bit equation. XOR of
    /// two rows already satisfying gf2's zero-unused-bits invariant can never
    /// violate it, so that work is pure overhead, and it is paid once per row
    /// operation in the reduction loop, the solver's hottest inner loop, where
    /// it measured at roughly 25% of total runtime. `store()` and `words()` are
    /// public parts of gf2's `BitStore` concept, so this stays within gf2's
    /// interface and inside `algebra/dense/`.
    /// @pre `other.width() == width()`.
    void xor_assign(const DenseRow& other) noexcept {
        assert(bits_.size() == other.bits_.size());
        Word* lhs = bits_.store();
        const Word* rhs = other.bits_.store();
        const std::size_t num_words = bits_.words();
        for (std::size_t i = 0; i < num_words; ++i) {
            lhs[i] ^= rhs[i];
        }
    }

    /// @brief Overwrite from `other`, of equal width, without allocating.
    void assign_from(const DenseRow& other) noexcept;

    bool equals(const DenseRow& other) const noexcept { return bits_ == other.bits_; }

    std::size_t hash() const noexcept;

    /// @brief Call `f(column)` for each set column, in ascending order.
    template <class F>
    void for_each_column(F&& f) const {
        for (auto column = leading_column(); column; column = next_column(*column)) {
            f(*column);
        }
    }

private:
    static constexpr std::size_t bits_per_word = sizeof(Word) * 8;

    /// @brief `log2(bits_per_word)`: shifts a column down to its word index.
    static constexpr std::size_t word_shift =
        static_cast<std::size_t>(std::countr_zero(bits_per_word));

    static constexpr std::size_t word_mask = bits_per_word - 1; ///< Bit offset within a word.

    Storage bits_;
};

} // namespace xorcle
