#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "algebra/algebra.h"
#include "algebra/equation.h"
#include "algebra/equation_impl.h"
#include "algebra/reduce_result.h"
#include "algebra/row.h"
#include "algebra/row_echelon_matrix.h"
#include "util/bitmap.h"

namespace xorcle {

/// @brief The one implementation of `RowEchelonMatrixStorage`, written against `Row`.
///
/// This is the port of the old tree's `REFMatrix`, with the sentinel returns
/// replaced by `ReduceResult` and the per-column scan in `add_row_get_index`
/// replaced by a popcount. Every reduction loop below is a direct, inlinable
/// call chain on `R`; the caller pays exactly one virtual call for the whole
/// loop.
///
/// The rows are held in an ordinary `std::vector`, not a matrix type: insertion
/// at an arbitrary index and removal of the first row are ordinary vector
/// operations, and `gf2::BitMatrix` can express neither.
/// The element type is `Equation` rather than `R` itself so that `row()`
/// can hand out a `const Equation&` without copying; each element owns exactly
/// one `R` through an `EquationImpl<R>`.
template <Row R>
class RowEchelonMatrixImpl final : public RowEchelonMatrixStorage {
public:
    /// @brief Construct an empty matrix over `num_vars` variables.
    explicit RowEchelonMatrixImpl(std::size_t num_vars)
        : pivot_columns_(num_vars + 1), column_to_row_(num_vars + 1, 0), num_vars_(num_vars) {}

    Algebra backend() const noexcept override { return R::algebra; }

    std::unique_ptr<RowEchelonMatrixStorage> clone() const override {
        return std::make_unique<RowEchelonMatrixImpl>(*this);
    }

    std::size_t rows() const noexcept override { return rows_.size(); }

    std::size_t num_vars() const noexcept override { return num_vars_; }

    const Equation& row(std::size_t index) const override {
        assert(index < rows_.size());
        return rows_[index];
    }

    const Bitmap& pivot_columns() const noexcept override { return pivot_columns_; }

    std::size_t row_for_pivot_column(std::size_t column) const override {
        assert(column < cols());
        assert(pivot_columns_.test(column) && "not a pivot column");
        return column_to_row_[column];
    }

    std::optional<std::size_t> add_row_get_index(const Equation& v) override {
        assert(v.size() == cols() && v.backend() == R::algebra);

        // Partially reduce a copy of `v` against the stored rows. The old tree
        // walked every column from 0 to num_vars calling `element()`; driving
        // the loop from `leading_column()` / `next_column()` visits only the set
        // columns and reaches the identical leading column.
        Equation reduced = v;
        R& reduced_row = row_of<R>(reduced);
        std::optional<std::size_t> leading = reduced_row.leading_column();
        while (leading && pivot_columns_.test(*leading)) {
            reduced_row.xor_assign(row_of<R>(rows_[column_to_row_[*leading]]));
            leading = reduced_row.next_column(*leading);
        }
        if (!leading) {
            return std::nullopt; // `v` already belongs to the span
        }

        // Row-echelon order means the insertion index is the number of pivots
        // strictly below the new leading column -- a popcount over the pivot
        // bitmap rather than the old O(num_vars) scan.
        const std::size_t leading_column = *leading;
        const std::size_t insert_index = pivot_columns_.count_below(leading_column);

        rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(insert_index), std::move(reduced));
        pivot_columns_.for_each_set([&](std::size_t pivot_column) {
            if (pivot_column > leading_column) {
                ++column_to_row_[pivot_column];
            }
        });
        pivot_columns_.set(leading_column);
        column_to_row_[leading_column] = insert_index;

        debug_assert_valid_representation();
        return insert_index;
    }

    void reduce(Equation& v) const override {
        assert(v.size() == cols() && v.backend() == R::algebra);
        R& row = row_of<R>(v);
        // `v` is mutated while its set columns are being walked; `next_column`
        // is re-evaluated against the live row, exactly as the old
        // `if_set_call` did.
        for (auto column = row.leading_column(); column; column = row.next_column(*column)) {
            if (pivot_columns_.test(*column)) {
                row.xor_assign(row_of<R>(rows_[column_to_row_[*column]]));
            }
        }
    }

    void reduce(Equation& v, Bitmap& coefficients_out) const override {
        assert(v.size() == cols() && v.backend() == R::algebra);
        coefficients_out.reset_width(rows_.size());
        R& row = row_of<R>(v);
        for (auto column = row.leading_column(); column; column = row.next_column(*column)) {
            if (pivot_columns_.test(*column)) {
                const std::size_t row_index = column_to_row_[*column];
                row.xor_assign(row_of<R>(rows_[row_index]));
                coefficients_out.set(row_index);
            }
        }
    }

    ReduceResult partial_reduce(Equation& v) const override {
        assert(v.size() == cols() && v.backend() == R::algebra);
        R& row = row_of<R>(v);
        std::optional<std::size_t> column = row.leading_column();
        while (true) {
            if (!column) {
                return ReduceResult::eq_true();
            }
            if (!pivot_columns_.test(*column)) {
                return classify_leading(*column);
            }
            row.xor_assign(row_of<R>(rows_[column_to_row_[*column]]));
            column = row.next_column(*column);
        }
    }

    ReduceResult partial_reduce_with_extra(Equation& v, const Equation& extra) const override {
        assert(v.size() == cols() && v.backend() == R::algebra);
        assert(extra.size() == cols() && extra.backend() == R::algebra);

        const R& extra_row = row_of<R>(extra);
        const std::optional<std::size_t> extra_leading = extra_row.leading_column();
        assert((!extra_leading || !pivot_columns_.test(*extra_leading)) &&
               "extra must already be partially reduced by this matrix");

        R& row = row_of<R>(v);
        std::optional<std::size_t> column = row.leading_column();
        while (true) {
            if (!column) {
                return ReduceResult::eq_true();
            }
            if (!pivot_columns_.test(*column)) {
                if (extra_leading && *column == *extra_leading) {
                    row.xor_assign(extra_row);
                    column = row.next_column(*column);
                    continue;
                }
                return classify_leading(*column);
            }
            row.xor_assign(row_of<R>(rows_[column_to_row_[*column]]));
            column = row.next_column(*column);
        }
    }

    void reduce_with_extra(Equation& v, const Equation& extra) const override {
        assert(v.size() == cols() && v.backend() == R::algebra);
        assert(extra.size() == cols() && extra.backend() == R::algebra);

        const R& extra_row = row_of<R>(extra);
        const std::optional<std::size_t> extra_leading = extra_row.leading_column();
        assert((!extra_leading || !pivot_columns_.test(*extra_leading)) &&
               "extra must already be partially reduced by this matrix");

        R& row = row_of<R>(v);
        for (auto column = row.leading_column(); column; column = row.next_column(*column)) {
            if (extra_leading && *column == *extra_leading) {
                row.xor_assign(extra_row);
            } else if (pivot_columns_.test(*column)) {
                row.xor_assign(row_of<R>(rows_[column_to_row_[*column]]));
            }
        }
    }

    Equation pop_first_row() override {
        assert(!rows_.empty() && "pop_first_row on an empty matrix");
        Equation first_row = std::move(rows_.front());
        const std::optional<std::size_t> leading = row_of<R>(first_row).leading_column();
        assert(leading && "stored row has no pivot");

        rows_.erase(rows_.begin());
        pivot_columns_.reset(*leading);
        pivot_columns_.for_each_set([&](std::size_t pivot_column) {
            --column_to_row_[pivot_column];
        });

        debug_assert_valid_representation();
        return first_row;
    }

    void pop_last_row() override {
        assert(!rows_.empty() && "pop_last_row on an empty matrix");
        const std::optional<std::size_t> leading = row_of<R>(rows_.back()).leading_column();
        assert(leading && "stored row has no pivot");

        rows_.pop_back();
        pivot_columns_.reset(*leading);

        debug_assert_valid_representation();
    }

    void solution(Bitmap& out) const override {
        // Every variable column is a pivot and the constant column is not, so
        // rows() == num_vars and row `i` has leading column `i`. Back-substitute
        // from the last row upward. This replaces the old tree's
        // `get_unique_solution`, which reduced the matrix and then read off a
        // whole column -- the one operation deliberately absent from this
        // interface.
        assert(rows_.size() == num_vars_);
        out.reset_width(num_vars_);
        for (std::size_t i = num_vars_; i-- > 0;) {
            const R& row = row_of<R>(rows_[i]);
            assert(row.leading_column() == std::optional<std::size_t>(i));
            bool value = row.test(num_vars_);
            for (auto column = row.next_column(i); column && *column < num_vars_;
                 column = row.next_column(*column)) {
                value ^= out.test(*column);
            }
            if (value) {
                out.set(i);
            }
        }
    }

private:
    std::size_t cols() const noexcept { return num_vars_ + 1; }

    /// @brief Turn an irreducible leading column into a `ReduceResult`.
    ///
    /// A leading column equal to `num_vars_` is the constant column, and since
    /// it is the last column nothing else can be set: the row is
    /// @f$\mathbf{1}@f$.
    ReduceResult classify_leading(std::size_t column) const noexcept {
        return column == num_vars_ ? ReduceResult::eq_false() : ReduceResult::leading(column);
    }

    /// @brief Check the representation invariants.
    ///
    /// Ported from the old tree's `debug_assert_valid_representation`, expressed
    /// over `Row`. Compiled out under `NDEBUG`.
    void debug_assert_valid_representation() const {
#ifndef NDEBUG
        assert(pivot_columns_.size() == cols());
        assert(column_to_row_.size() == cols());
        assert(pivot_columns_.count() == rows_.size());

        std::optional<std::size_t> previous_pivot;
        for (std::size_t row_index = 0; row_index < rows_.size(); ++row_index) {
            const R& current = row_of<R>(rows_[row_index]);
            assert(current.width() == cols());

            const std::optional<std::size_t> leading = current.leading_column();
            assert(leading && *leading < cols());
            if (previous_pivot) {
                assert(*previous_pivot < *leading);
            }
            assert(pivot_columns_.test(*leading));
            assert(column_to_row_[*leading] == row_index);
            previous_pivot = leading;
        }

        pivot_columns_.for_each_set([&](std::size_t pivot_column) {
            assert(column_to_row_[pivot_column] < rows_.size());
            assert(row_of<R>(rows_[column_to_row_[pivot_column]]).leading_column() ==
                   std::optional<std::size_t>(pivot_column));
        });
#endif
    }

    std::vector<Equation> rows_;
    Bitmap pivot_columns_;
    std::vector<std::size_t> column_to_row_;
    std::size_t num_vars_;
};

} // namespace xorcle
