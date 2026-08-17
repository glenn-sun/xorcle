#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "algebra/algebra.h"
#include "algebra/reduce_result.h"

namespace xorcle {

/// @brief The sorted-index `Row` implementation.
///
/// A row is the ascending list of its set columns. This backend exists to keep
/// the algebra interface honest -- an interface designed against a single
/// implementation is reliably the wrong interface. It is expected to be slower
/// than the dense backend, and runs the same unit tests.
class SparseRow {
public:
    static constexpr Algebra algebra = Algebra::sparse;

    using Column = std::uint32_t;

    /// @brief Construct the all-zero row of width `num_vars + 1`.
    explicit SparseRow(std::size_t width) : width_(width) {}

    std::size_t width() const noexcept { return width_; }

    /// @brief Resize to `width` columns, dropping any set column at or above it.
    void resize(std::size_t width);

    void clear() noexcept { columns_.clear(); }

    /// @brief Clear, then set each listed column; each must be below `width()`.
    void assign_columns(std::span<const std::size_t> columns);

    /// @pre `i < width()`.
    bool test(std::size_t i) const noexcept {
        assert(i < width_);
        return std::binary_search(columns_.begin(), columns_.end(), static_cast<Column>(i));
    }

    /// @pre `i < width()`.
    void set(std::size_t i);

    /// @brief First set column, or `std::nullopt` if none is set.
    std::optional<std::size_t> leading_column() const noexcept {
        if (columns_.empty()) {
            return std::nullopt;
        }
        return columns_.front();
    }

    /// @brief First set column strictly after `i`, or `std::nullopt`.
    std::optional<std::size_t> next_column(std::size_t i) const noexcept {
        const auto it = std::upper_bound(columns_.begin(), columns_.end(), static_cast<Column>(i));
        if (it == columns_.end()) {
            return std::nullopt;
        }
        return *it;
    }

    bool is_zero() const noexcept { return columns_.empty(); }

    /// @brief Classify as @f$\mathbf{0}@f$, @f$\mathbf{1}@f$, or a leading
    /// column, in one size-and-front check rather than separate emptiness and
    /// constant-column queries. The constant column is the highest, so a leading
    /// column at `width_ - 1` means it is the only set column.
    ReduceResult classify() const noexcept {
        if (columns_.empty()) {
            return ReduceResult::eq_true();
        }
        const std::size_t leading = columns_.front();
        if (leading + 1 == width_) {
            return ReduceResult::eq_false();
        }
        return ReduceResult::leading(leading);
    }

    /// @brief Add another row of equal width into this one over GF(2).
    void xor_assign(const SparseRow& other);

    /// @brief Overwrite from `other`, of equal width, without allocating.
    void assign_from(const SparseRow& other);

    bool equals(const SparseRow& other) const noexcept {
        return width_ == other.width_ && columns_ == other.columns_;
    }

    std::size_t hash() const noexcept;

    /// @brief Call `f(column)` for each set column, in ascending order.
    template <class F>
    void for_each_column(F&& f) const {
        for (const Column column : columns_) {
            f(column);
        }
    }

private:
    std::size_t width_;
    std::vector<Column> columns_;
    std::vector<Column> scratch_; ///< Merge buffer for `xor_assign`, kept to avoid allocating.
};

} // namespace xorcle
