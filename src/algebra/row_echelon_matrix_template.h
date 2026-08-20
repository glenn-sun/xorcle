#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/equation_template.h"
#include "algebra/gf2_vector.h"
#include "algebra/reduce_result.h"
#include "algebra/row_echelon_matrix.h"
#include "util/index_set.h"

namespace xorcle {

/// @brief Implements `RowEchelonMatrixInterface` against any `GF2Vector`.
///
/// Every reduction loop below is a direct, inlinable call chain on `V`; the
/// caller pays exactly one virtual call for the whole loop.
///
/// The rows are held in an ordinary `std::vector`, not a matrix type: insertion
/// at an arbitrary index and removal of the first row are ordinary vector
/// operations that a packed bit-matrix cannot express. The element type is
/// `Equation` rather than `V` so that `row()` can hand out a `const Equation&`
/// without copying; each element owns exactly one `V` via an
/// `EquationTemplate<V>`.
template <GF2Vector V>
class RowEchelonMatrixTemplate final : public RowEchelonMatrixInterface {
public:
    explicit RowEchelonMatrixTemplate(std::size_t num_vars)
        : pivot_columns_(num_vars + 1), column_to_row_(num_vars + 1, 0), num_vars_(num_vars) {}

    EquationRepr repr() const noexcept override { return V::repr; }

    std::unique_ptr<RowEchelonMatrixInterface> clone() const override {
        return std::make_unique<RowEchelonMatrixTemplate>(*this);
    }

    std::size_t rows() const noexcept override { return rows_.size(); }

    std::size_t num_vars() const noexcept override { return num_vars_; }

    const Equation& row(std::size_t index) const override {
        assert(index < rows_.size());
        return rows_[index];
    }

    const IndexSet& pivot_columns() const noexcept override { return pivot_columns_; }

    std::optional<std::size_t> add_row_get_index(const Equation& v) override {
        assert(v.num_bits() == cols() && v.repr() == V::repr);

        // Partially reduce a copy of `v` against the stored rows.
        Equation reduced = v;
        V& reduced_row = gf2_vector_of<V>(reduced);
        std::optional<std::size_t> leading = reduced_row.leading_bit();
        while (leading && pivot_columns_.test(*leading)) {
            reduced_row.plus_assign(gf2_vector_of<V>(rows_[column_to_row_[*leading]]));
            leading = reduced_row.next_bit(*leading);
        }
        if (!leading) {
            return std::nullopt; // `v` already belongs to the span
        }

        // Row-echelon order means the insertion index is the number of pivots
        // strictly below the new leading column
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
        assert(v.num_bits() == cols() && v.repr() == V::repr);
        V& row = gf2_vector_of<V>(v);
        // `v` is mutated while its set columns are being walked, so `next_bit`
        // must be re-evaluated against the live row each time.
        for (auto column = row.leading_bit(); column; column = row.next_bit(*column)) {
            if (pivot_columns_.test(*column)) {
                row.plus_assign(gf2_vector_of<V>(rows_[column_to_row_[*column]]));
            }
        }
    }

    void reduce(Equation& v, IndexSet& coefficients_out) const override {
        assert(v.num_bits() == cols() && v.repr() == V::repr);
        coefficients_out.reset_size(rows_.size());
        V& row = gf2_vector_of<V>(v);
        for (auto column = row.leading_bit(); column; column = row.next_bit(*column)) {
            if (pivot_columns_.test(*column)) {
                const std::size_t row_index = column_to_row_[*column];
                row.plus_assign(gf2_vector_of<V>(rows_[row_index]));
                coefficients_out.set(row_index);
            }
        }
    }

    ReduceResult partial_reduce(Equation& v) const override {
        assert(v.num_bits() == cols() && v.repr() == V::repr);
        V& row = gf2_vector_of<V>(v);
        std::optional<std::size_t> column = row.leading_bit();
        while (true) {
            if (!column) {
                return ReduceResult::eq_true();
            }
            if (!pivot_columns_.test(*column)) {
                return classify_column(*column);
            }
            row.plus_assign(gf2_vector_of<V>(rows_[column_to_row_[*column]]));
            column = row.next_bit(*column);
        }
    }

    ReduceResult partial_reduce_with_extra(Equation& v, const Equation& extra) const override {
        assert(v.num_bits() == cols() && v.repr() == V::repr);
        assert(extra.num_bits() == cols() && extra.repr() == V::repr);

        const V& extra_row = gf2_vector_of<V>(extra);
        const std::optional<std::size_t> extra_leading = extra_row.leading_bit();
        assert((!extra_leading || !pivot_columns_.test(*extra_leading)) &&
               "extra must already be partially reduced by this matrix");

        V& row = gf2_vector_of<V>(v);
        std::optional<std::size_t> column = row.leading_bit();
        while (true) {
            if (!column) {
                return ReduceResult::eq_true();
            }
            if (!pivot_columns_.test(*column)) {
                if (extra_leading && *column == *extra_leading) {
                    row.plus_assign(extra_row);
                    column = row.next_bit(*column);
                    continue;
                }
                return classify_column(*column);
            }
            row.plus_assign(gf2_vector_of<V>(rows_[column_to_row_[*column]]));
            column = row.next_bit(*column);
        }
    }

    Equation pop_first_row() override {
        assert(!rows_.empty() && "pop_first_row on an empty matrix");
        Equation first_row = std::move(rows_.front());
        const std::optional<std::size_t> leading = gf2_vector_of<V>(first_row).leading_bit();
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
        const std::optional<std::size_t> leading = gf2_vector_of<V>(rows_.back()).leading_bit();
        assert(leading && "stored row has no pivot");

        rows_.pop_back();
        pivot_columns_.reset(*leading);

        debug_assert_valid_representation();
    }

    IndexSet solution() const override {
        assert(rows_.size() == num_vars_);
        IndexSet out(num_vars_);
        for (std::size_t i = num_vars_; i-- > 0;) {
            const V& row = gf2_vector_of<V>(rows_[i]);
            assert(row.leading_bit() == std::optional<std::size_t>(i));
            bool value = row.get_bit(num_vars_); // the row's constant bit
            for (auto column = row.next_bit(i); column && *column < num_vars_;
                 column = row.next_bit(*column)) {
                value ^= out.test(*column);
            }
            if (value) {
                out.set(i);
            }
        }
        return out;
    }

private:
    std::size_t cols() const noexcept { return num_vars_ + 1; }

    ReduceResult classify_column(std::size_t column) const noexcept {
        return column == num_vars_ ? ReduceResult::eq_false() : ReduceResult::leading(column);
    }

    /// @brief Check representation invariants for `RowEchelonMatrix`.
    /// Compiled out under `NDEBUG`.
    void debug_assert_valid_representation() const {
#ifndef NDEBUG
        assert(pivot_columns_.size() == cols());
        assert(column_to_row_.size() == cols());
        assert(pivot_columns_.count() == rows_.size());

        std::optional<std::size_t> previous_pivot;
        for (std::size_t row_index = 0; row_index < rows_.size(); ++row_index) {
            const V& current = gf2_vector_of<V>(rows_[row_index]);
            assert(current.num_bits() == cols());

            const std::optional<std::size_t> leading = current.leading_bit();
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
            assert(gf2_vector_of<V>(rows_[column_to_row_[pivot_column]]).leading_bit() ==
                   std::optional<std::size_t>(pivot_column));
        });
#endif
    }

    std::vector<Equation> rows_;
    IndexSet pivot_columns_;
    std::vector<std::size_t> column_to_row_;
    std::size_t num_vars_;
};

} // namespace xorcle
