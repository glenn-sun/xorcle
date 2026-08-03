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
/// This header and `dense_row.cpp` are the only files in the project permitted
/// to include gf2. The word type is spelled explicitly: `gf2::BitVector<>` defaults
/// to `std::size_t`, which on this platform is `unsigned long` while `gf2::u64`
/// is `unsigned long long`, and gf2's binary operators require identical word
/// types, so mixing the two silently fails to compile or compares unequal.
class DenseRow {
public:
    /// @brief The backend this row belongs to.
    static constexpr Algebra algebra = Algebra::dense;

    /// @brief The underlying bit-vector type.
    using Storage = gf2::BitVector<gf2::u64>;

    /// @brief The machine word the bits are packed into.
    using Word = Storage::word_type;

    /// @brief Construct the all-zero row of the given width.
    /// @param width Number of columns, i.e. `num_vars + 1`.
    explicit DenseRow(std::size_t width) : bits_(width) {}

    /// @brief Number of columns.
    std::size_t width() const noexcept { return bits_.size(); }

    /// @brief Resize to `width` columns, zeroing any new ones.
    void resize(std::size_t width) { bits_.resize(width); }

    /// @brief Set every column to zero, keeping the width.
    void clear() noexcept { gf2::set_all(bits_, false); }

    /// @brief Clear, then set each listed column.
    /// @param columns Columns to set; each must be below `width()`.
    void assign_columns(std::span<const std::size_t> columns) {
        clear();
        for (const std::size_t column : columns) {
            set(column);
        }
    }

    /// @brief Whether column `i` is set.
    /// @pre `i < width()`.
    bool test(std::size_t i) const noexcept { return bits_.get(i); }

    /// @brief Set column `i`.
    /// @pre `i < width()`.
    void set(std::size_t i) noexcept { gf2::set(bits_, i, true); }

    /// @brief First set column, or `std::nullopt` if none is set.
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
    /// exactly, but gf2 masks the starting word with a `reset_bits` call guarded
    /// by an `i == word_index` test *inside* its scan loop, and re-checks the
    /// word index against `words()` after already checking the bit index against
    /// `size()`. Written out, the mask is one branch-free shift and the loop
    /// carries no per-word conditional. This runs once per row operation in the
    /// reduction loop, where those few instructions are a measurable fraction of
    /// the iteration (the granularity is right,
    /// but the constant still has to be paid attention to).
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

    /// @brief Whether no column is set.
    bool is_zero() const noexcept { return bits_.none(); }

    /// @brief Classify as @f$\mathbf{0}@f$, @f$\mathbf{1}@f$, or a leading column.
    ///
    /// A direct word scan: `leading_column()` already walks whole words, and a
    /// leading column at `width() - 1` can only mean the constant column alone
    /// is set. Never composed from separate emptiness and constant-column
    /// queries.
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
    /// final word on every call; `clean()` is a read-modify-write that fires
    /// whenever `size() % 64 != 0`, which for an @f$n+1@f$-bit equation is
    /// essentially always. XOR of two rows that already satisfy gf2's
    /// zero-unused-bits invariant can never violate it, so that work is pure
    /// overhead -- and it is paid once per row operation in the reduction loop,
    /// the solver's hottest inner loop. `store()` and `words()` are public parts
    /// of gf2's `BitStore` concept, so this stays inside stock gf2 and inside
    /// `algebra/dense/`.
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

    /// @brief Overwrite this row's contents from `other`, without allocating.
    /// @pre `other.width() == width()`.
    void assign_from(const DenseRow& other) noexcept;

    /// @brief Whether two rows are equal.
    bool equals(const DenseRow& other) const noexcept { return bits_ == other.bits_; }

    /// @brief Hash of the row contents.
    std::size_t hash() const noexcept;

    /// @brief Call `f(column)` for each set column, in ascending order.
    template <class F>
    void for_each_column(F&& f) const {
        for (auto column = leading_column(); column; column = next_column(*column)) {
            f(*column);
        }
    }

private:
    /// @brief Bits packed into one machine word.
    static constexpr std::size_t bits_per_word = sizeof(Word) * 8;

    /// @brief `log2(bits_per_word)`, turning a column into a word index.
    static constexpr std::size_t word_shift =
        static_cast<std::size_t>(std::countr_zero(bits_per_word));

    /// @brief Mask selecting a column's bit offset within its word.
    static constexpr std::size_t word_mask = bits_per_word - 1;

    Storage bits_;
};

} // namespace xorcle
