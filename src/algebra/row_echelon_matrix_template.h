#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/gf2_vector.h"
#include "algebra/reduce_result.h"
#include "util/index_set.h"

namespace xorcle {

/// @brief The abstract interface behind `RowEchelonMatrix`.
/// Contains common functionality involving loops over rows, to avoid virtual
/// dispatch at low levels.
class RowEchelonMatrixInterface {
public:
    virtual ~RowEchelonMatrixInterface() = default;

    virtual EquationRepr repr() const noexcept = 0;

    virtual std::unique_ptr<RowEchelonMatrixInterface> clone() const = 0;

    virtual std::size_t rows() const noexcept = 0;

    virtual std::size_t cols() const noexcept = 0;

    virtual const Equation& row(std::size_t index) const = 0;

    virtual const IndexSet& pivot_columns() const noexcept = 0;

    virtual std::optional<std::size_t> add_row_get_index(const Equation& v) = 0;

    virtual void reduce(Equation& v) const = 0;

    virtual IndexSet reduce_get_coefficients(Equation& v) const = 0;

    /// @brief Reduce `v` in place only until its leading column is not a pivot,
    /// reading the last column as the constant.
    virtual ReduceResult partial_reduce(Equation& v) const = 0;

    /// @brief Partially reduce `v` against this matrix augmented with `extra`,
    /// reading the last column as the constant.
    virtual ReduceResult partial_reduce_with_extra(Equation& v, const Equation& extra) const = 0;

    virtual Equation pop_first_row() = 0;

    virtual void pop_last_row() = 0;

    /// @brief The unique solution of the stored system, one bit per column but
    /// the last, which is read as the constant.
    virtual IndexSet solution() const = 0;
};

/// @brief Implements `RowEchelonMatrixInterface` against any `GF2Vector`.
template <GF2Vector V>
class RowEchelonMatrixTemplate final : public RowEchelonMatrixInterface {
public:
    explicit RowEchelonMatrixTemplate(std::size_t cols)
        : pivot_columns_(cols), column_to_row_(cols, 0), cols_(cols) {}

    EquationRepr repr() const noexcept override { return V::repr; }

    std::unique_ptr<RowEchelonMatrixInterface> clone() const override {
        return std::make_unique<RowEchelonMatrixTemplate>(*this);
    }

    std::size_t rows() const noexcept override { return rows_.size(); }

    std::size_t cols() const noexcept override { return cols_; }

    const Equation& row(std::size_t index) const override { return rows_[index]; }

    const IndexSet& pivot_columns() const noexcept override { return pivot_columns_; }

    std::optional<std::size_t> add_row_get_index(const Equation& v) override {
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
        V& row = gf2_vector_of<V>(v);
        // `v` is mutated while its set columns are being walked, so `next_bit`
        // must be re-evaluated against the live row each time.
        for (auto column = row.leading_bit(); column; column = row.next_bit(*column)) {
            if (pivot_columns_.test(*column)) {
                row.plus_assign(gf2_vector_of<V>(rows_[column_to_row_[*column]]));
            }
        }
    }

    IndexSet reduce_get_coefficients(Equation& v) const override {
        IndexSet coefficients(rows_.size());
        V& row = gf2_vector_of<V>(v);
        for (auto column = row.leading_bit(); column; column = row.next_bit(*column)) {
            if (pivot_columns_.test(*column)) {
                const std::size_t row_index = column_to_row_[*column];
                row.plus_assign(gf2_vector_of<V>(rows_[row_index]));
                coefficients.set(row_index);
            }
        }
        return coefficients;
    }

    ReduceResult partial_reduce(Equation& v) const override {
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
        const V& extra_row = gf2_vector_of<V>(extra);
        const std::optional<std::size_t> extra_leading = extra_row.leading_bit();
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
        Equation first_row = std::move(rows_.front());
        const std::optional<std::size_t> leading = gf2_vector_of<V>(first_row).leading_bit();
        // Stronger than the caller's `rows() > 0`: a structural invariant of a
        // row-echelon matrix, which never stores a zero row.
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
        const std::optional<std::size_t> leading = gf2_vector_of<V>(rows_.back()).leading_bit();
        // Stronger than the caller's `rows() > 0`; see `pop_first_row()`.
        assert(leading && "stored row has no pivot");

        rows_.pop_back();
        pivot_columns_.reset(*leading);

        debug_assert_valid_representation();
    }

    IndexSet solution() const override {
        const std::size_t constant_column = cols_ - 1;
        IndexSet out(constant_column);
        for (std::size_t i = constant_column; i-- > 0;) {
            const V& row = gf2_vector_of<V>(rows_[i]);
            // Stronger than `RowSpace::unique_solution()`'s full-rank check:
            // full rank forces one pivot per variable column, and this pins each
            // one to its own row.
            assert(row.leading_bit() == std::optional<std::size_t>(i) &&
                   "a full-rank row space must have row i leading on column i");
            bool value = row.get_bit(constant_column);
            for (auto column = row.next_bit(i); column && *column < constant_column;
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
    ReduceResult classify_column(std::size_t column) const noexcept {
        return column + 1 == cols_ ? ReduceResult::eq_false() : ReduceResult::leading(column);
    }

    /// @brief Check representation invariants for `RowEchelonMatrix`.
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
    std::size_t cols_;
};

} // namespace xorcle
