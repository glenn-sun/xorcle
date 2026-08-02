#pragma once

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
    std::optional<std::size_t> leading_column() const noexcept { return bits_.first_set(); }

    /// @brief First set column strictly after `i`, or `std::nullopt`.
    /// @pre `i < width()`.
    std::optional<std::size_t> next_column(std::size_t i) const noexcept {
        return bits_.next_set(i);
    }

    /// @brief Whether no column is set.
    bool is_zero() const noexcept { return bits_.none(); }

    /// @brief Classify as @f$\mathbf{0}@f$, @f$\mathbf{1}@f$, or a leading column.
    ///
    /// A direct word scan: `first_set()` already walks whole words, and a
    /// leading column at `width() - 1` can only mean the constant column alone
    /// is set. Never composed from separate emptiness and constant-column
    /// queries.
    ReduceResult classify() const noexcept {
        const std::optional<std::size_t> leading = bits_.first_set();
        if (!leading) {
            return ReduceResult::eq_true();
        }
        if (*leading + 1 == bits_.size()) {
            return ReduceResult::eq_false();
        }
        return ReduceResult::leading(*leading);
    }

    /// @brief Add another row into this one over GF(2).
    /// @pre `other.width() == width()`.
    void xor_assign(const DenseRow& other) noexcept {
        assert(bits_.size() == other.bits_.size());
        bits_ ^= other.bits_;
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
        for (auto column = bits_.first_set(); column; column = bits_.next_set(*column)) {
            f(*column);
        }
    }

private:
    Storage bits_;
};

} // namespace xorcle
