#pragma once

#ifndef XORCLE_COL_IDX_TO_ROW_DIVISOR
#define XORCLE_COL_IDX_TO_ROW_DIVISOR 8
#endif

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

/// @brief The abstract interface behind `RowEchelonMatrix`, containing common
/// functionality involving loops over rows.
class RowEchelonMatrixInterface {
public:
    virtual ~RowEchelonMatrixInterface() = default;

    virtual EquationRepr repr() const noexcept = 0;

    virtual std::unique_ptr<RowEchelonMatrixInterface> clone() const = 0;

    virtual std::size_t rows() const noexcept = 0;

    virtual std::size_t cols() const noexcept = 0;

    virtual const Equation& row(std::size_t row_idx) const = 0;

    virtual const IndexSet& pivot_col_idxs() const noexcept = 0;

    virtual std::optional<std::size_t> add_row_get_index(Equation v) = 0;

    virtual void reduce(Equation& v) const = 0;

    virtual IndexSet reduce_get_coefficients(Equation& v) const = 0;

    /// @brief Reduce `v` in place only until its leading column is not a pivot,
    /// reading the last column as the constant.
    virtual ReduceResult partial_reduce(Equation& v) const = 0;

    /// @brief Partially reduce `v` against this matrix augmented with `extra`,
    /// reading the last column as the constant.
    virtual ReduceResult partial_reduce_with_extra(Equation& v, const Equation& extra) const = 0;

    /// @brief Remove the row at `row_idx`, returning it.
    virtual Equation pop_row(std::size_t row_idx) = 0;

    virtual void pop_last_row() = 0;

    /// @brief The unique solution of the stored system, one bit per column but
    /// the last, which is read as the constant.
    virtual IndexSet solution() const = 0;
};

/// @brief Implements `RowEchelonMatrixInterface` against any `GF2Vector`.
template <GF2Vector V>
class RowEchelonMatrixTemplate final : public RowEchelonMatrixInterface {
public:
    /// @brief A table indexed by column costs `cols()` to build, so it pays off
    /// once the rows occupy this fraction of the columns.
    static constexpr std::size_t col_idx_to_row_divisor = XORCLE_COL_IDX_TO_ROW_DIVISOR;

    explicit RowEchelonMatrixTemplate(std::size_t cols)
        : pivot_col_idxs_(cols), cols_(cols) {}

    // `pivot_row_` points into another matrix's rows, so a copy has to rebuild it.
    RowEchelonMatrixTemplate(const RowEchelonMatrixTemplate& other)
        : rows_(other.rows_), pivot_col_idxs_(other.pivot_col_idxs_),
          col_idx_to_row_idx_(other.col_idx_to_row_idx_), cols_(other.cols_) {
        if (!other.col_idx_to_row_.empty()) {
            build_col_idx_to_row();
        }
    }

    RowEchelonMatrixTemplate& operator=(const RowEchelonMatrixTemplate& other) {
        if (this != &other) {
            rows_ = other.rows_;
            pivot_col_idxs_ = other.pivot_col_idxs_;
            col_idx_to_row_idx_ = other.col_idx_to_row_idx_;
            cols_ = other.cols_;
            col_idx_to_row_.clear();
            if (!other.col_idx_to_row_.empty()) {
                build_col_idx_to_row();
            }
        }
        return *this;
    }

    RowEchelonMatrixTemplate(RowEchelonMatrixTemplate&&) = default;
    RowEchelonMatrixTemplate& operator=(RowEchelonMatrixTemplate&&) = default;

    EquationRepr repr() const noexcept override { return V::repr; }

    std::unique_ptr<RowEchelonMatrixInterface> clone() const override {
        return std::make_unique<RowEchelonMatrixTemplate>(*this);
    }

    std::size_t rows() const noexcept override { return rows_.size(); }

    std::size_t cols() const noexcept override { return cols_; }

    const Equation& row(std::size_t row_idx) const override { return rows_[row_idx]; }

    const IndexSet& pivot_col_idxs() const noexcept override { return pivot_col_idxs_; }

    std::optional<std::size_t> add_row_get_index(Equation v) override {
        // `v` is the caller's own copy, so it is reduced in place and stored.
        V& reduced_row = gf2_vector_of<V>(v);
        std::optional<std::size_t> leading_col_idx = reduced_row.leading_bit();
        while (leading_col_idx && pivot_col_idxs_.test(*leading_col_idx)) {
            leading_col_idx = reduced_row.plus_assign_and_next_bit(
                gf2_vector_of<V>(rows_[row_idx_for_col_idx(*leading_col_idx)]), *leading_col_idx);
        }
        if (!leading_col_idx) {
            return std::nullopt; // `v` already belongs to the span
        }

        // Row-echelon order means the insertion row_idx is the number of pivots
        // strictly below the new leading column
        const std::size_t new_pivot_col_idx = *leading_col_idx;
        const std::size_t insert_row_idx = pivot_col_idxs_.count_below(new_pivot_col_idx);

        rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(insert_row_idx), std::move(v));
        if (!col_idx_to_row_idx_.empty()) {
            pivot_col_idxs_.for_each_set([&](std::size_t pivot_col_idx) {
                if (pivot_col_idx > new_pivot_col_idx) {
                    ++col_idx_to_row_idx_[pivot_col_idx];
                }
            });
        }
        pivot_col_idxs_.set(new_pivot_col_idx);
        if (!col_idx_to_row_idx_.empty()) {
            col_idx_to_row_idx_[new_pivot_col_idx] = insert_row_idx;
        }
        // Inserting moves handles, never the equations they own, so the entries
        // for the other pivot columns stay valid.
        if (!col_idx_to_row_.empty()) {
            col_idx_to_row_[new_pivot_col_idx] = &gf2_vector_of<V>(rows_[insert_row_idx]);
        } else if (rows_.size() * col_idx_to_row_divisor >= cols_) {
            build_col_idx_to_row();
        }

        debug_assert_valid_representation();
        return insert_row_idx;
    }

    void reduce(Equation& v) const override {
        V& row = gf2_vector_of<V>(v);
        // `v` is mutated while its set columns are being walked, so `next_bit`
        // must be re-evaluated against the live row each time.
        for (auto col_idx = row.leading_bit(); col_idx; col_idx = row.next_bit(*col_idx)) {
            if (pivot_col_idxs_.test(*col_idx)) {
                row.plus_assign(gf2_vector_of<V>(rows_[row_idx_for_col_idx(*col_idx)]));
            }
        }
    }

    IndexSet reduce_get_coefficients(Equation& v) const override {
        IndexSet coefficients(rows_.size());
        V& row = gf2_vector_of<V>(v);
        for (auto col_idx = row.leading_bit(); col_idx; col_idx = row.next_bit(*col_idx)) {
            if (pivot_col_idxs_.test(*col_idx)) {
                const std::size_t row_idx = row_idx_for_col_idx(*col_idx);
                row.plus_assign(gf2_vector_of<V>(rows_[row_idx]));
                coefficients.set(row_idx);
            }
        }
        return coefficients;
    }

    ReduceResult partial_reduce(Equation& v) const override {
        return col_idx_to_row_.empty() ? partial_reduce_impl<false>(v) : partial_reduce_impl<true>(v);
    }

    ReduceResult partial_reduce_with_extra(Equation& v, const Equation& extra) const override {
        return col_idx_to_row_.empty() ? partial_reduce_with_extra_impl<false>(v, extra)
                                  : partial_reduce_with_extra_impl<true>(v, extra);
    }

    Equation pop_row(std::size_t row_idx) override {
        Equation popped_row = std::move(rows_[row_idx]);
        const std::optional<std::size_t> leading_col_idx =
            gf2_vector_of<V>(popped_row).leading_bit();
        // Stronger than the caller's `rows() > 0`: a structural invariant of a
        // row-echelon matrix, which never stores a zero row.
        assert(leading_col_idx && "stored row has no pivot");

        rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(row_idx));
        pivot_col_idxs_.reset(*leading_col_idx);
        if (!col_idx_to_row_.empty()) {
            col_idx_to_row_[*leading_col_idx] = nullptr;
        }
        // Only the rows above the hole moved down.
        if (!col_idx_to_row_idx_.empty()) {
            pivot_col_idxs_.for_each_set([&](std::size_t pivot_col_idx) {
                if (pivot_col_idx > *leading_col_idx) {
                    --col_idx_to_row_idx_[pivot_col_idx];
                }
            });
        }

        debug_assert_valid_representation();
        return popped_row;
    }

    void pop_last_row() override {
        const std::optional<std::size_t> leading_col_idx = gf2_vector_of<V>(rows_.back()).leading_bit();
        // Stronger than the caller's `rows() > 0`; see `pop_row()`.
        assert(leading_col_idx && "stored row has no pivot");

        rows_.pop_back();
        pivot_col_idxs_.reset(*leading_col_idx);
        if (!col_idx_to_row_.empty()) {
            col_idx_to_row_[*leading_col_idx] = nullptr;
        }

        debug_assert_valid_representation();
    }

    IndexSet solution() const override {
        const std::size_t constant_col_idx = cols_ - 1;
        IndexSet out(constant_col_idx);
        for (std::size_t i = constant_col_idx; i-- > 0;) {
            const V& row = gf2_vector_of<V>(rows_[i]);
            // Stronger than `RowSpace::unique_solution()`'s full-rank check:
            // full rank forces one pivot per variable column, and this pins each
            // one to its own row.
            assert(row.leading_bit() == std::optional<std::size_t>(i) &&
                   "a full-rank row space must have row i leading on column i");
            bool value = row.get_bit(constant_col_idx);
            for (auto col_idx = row.next_bit(i); col_idx && *col_idx < constant_col_idx;
                 col_idx = row.next_bit(*col_idx)) {
                value ^= out.test(*col_idx);
            }
            if (value) {
                out.set(i);
            }
        }
        return out;
    }

private:
    template <bool FromColIdxToRow>
    const V& row_for_col_idx(std::size_t col_idx) const noexcept {
        if constexpr (FromColIdxToRow) {
            return *col_idx_to_row_[col_idx];
        } else {
            return gf2_vector_of<V>(rows_[row_idx_for_col_idx(col_idx)]);
        }
    }

    template <bool FromColIdxToRow>
    ReduceResult partial_reduce_impl(Equation& v) const {
        V& row = gf2_vector_of<V>(v);
        std::optional<std::size_t> col_idx = row.leading_bit();
        while (true) {
            if (!col_idx) {
                return ReduceResult::eq_true();
            }
            if (!pivot_col_idxs_.test(*col_idx)) {
                return classify_col_idx(*col_idx);
            }
            col_idx = row.plus_assign_and_next_bit(row_for_col_idx<FromColIdxToRow>(*col_idx),
                                                   *col_idx);
        }
    }

    template <bool FromColIdxToRow>
    ReduceResult partial_reduce_with_extra_impl(Equation& v, const Equation& extra) const {
        const V& extra_row = gf2_vector_of<V>(extra);
        const std::optional<std::size_t> extra_leading_col_idx = extra_row.leading_bit();
        V& row = gf2_vector_of<V>(v);
        std::optional<std::size_t> col_idx = row.leading_bit();
        while (true) {
            if (!col_idx) {
                return ReduceResult::eq_true();
            }
            if (!pivot_col_idxs_.test(*col_idx)) {
                if (extra_leading_col_idx && *col_idx == *extra_leading_col_idx) {
                    col_idx = row.plus_assign_and_next_bit(extra_row, *col_idx);
                    continue;
                }
                return classify_col_idx(*col_idx);
            }
            col_idx = row.plus_assign_and_next_bit(row_for_col_idx<FromColIdxToRow>(*col_idx),
                                                   *col_idx);
        }
    }

    /// @brief The row holding the pivot in `col_idx`.
    std::size_t row_idx_for_col_idx(std::size_t col_idx) const noexcept {
        return col_idx_to_row_idx_.empty() ? pivot_col_idxs_.count_below(col_idx)
                                           : col_idx_to_row_idx_[col_idx];
    }

    void build_col_idx_to_row() {
        col_idx_to_row_idx_.assign(cols_, 0);
        pivot_col_idxs_.for_each_set([&](std::size_t pivot_col_idx) {
            col_idx_to_row_idx_[pivot_col_idx] = pivot_col_idxs_.count_below(pivot_col_idx);
        });
        col_idx_to_row_.assign(cols_, nullptr);
        pivot_col_idxs_.for_each_set([&](std::size_t pivot_col_idx) {
            col_idx_to_row_[pivot_col_idx] = &gf2_vector_of<V>(rows_[row_idx_for_col_idx(pivot_col_idx)]);
        });
    }

    ReduceResult classify_col_idx(std::size_t col_idx) const noexcept {
        return col_idx + 1 == cols_ ? ReduceResult::eq_false() : ReduceResult::leading(col_idx);
    }

    /// @brief Check representation invariants for `RowEchelonMatrix`.
    void debug_assert_valid_representation() const {
#ifndef NDEBUG
        assert(pivot_col_idxs_.size() == cols());
        assert(col_idx_to_row_idx_.empty() || col_idx_to_row_idx_.size() == cols());
        assert(pivot_col_idxs_.count() == rows_.size());

        std::optional<std::size_t> previous_pivot;
        for (std::size_t row_idx = 0; row_idx < rows_.size(); ++row_idx) {
            const V& current = gf2_vector_of<V>(rows_[row_idx]);
            assert(current.num_bits() == cols());

            const std::optional<std::size_t> leading_col_idx = current.leading_bit();
            assert(leading_col_idx && *leading_col_idx < cols());
            if (previous_pivot) {
                assert(*previous_pivot < *leading_col_idx);
            }
            assert(pivot_col_idxs_.test(*leading_col_idx));
            assert(row_idx_for_col_idx(*leading_col_idx) == row_idx);
            previous_pivot = leading_col_idx;
        }

        pivot_col_idxs_.for_each_set([&](std::size_t pivot_col_idx) {
            assert(row_idx_for_col_idx(pivot_col_idx) < rows_.size());
            assert(row_idx_for_col_idx(pivot_col_idx) == pivot_col_idxs_.count_below(pivot_col_idx));
            assert(gf2_vector_of<V>(rows_[row_idx_for_col_idx(pivot_col_idx)]).leading_bit() ==
                   std::optional<std::size_t>(pivot_col_idx));
            assert(col_idx_to_row_.empty() ||
                   col_idx_to_row_[pivot_col_idx] ==
                       &gf2_vector_of<V>(rows_[row_idx_for_col_idx(pivot_col_idx)]));
        });
#endif
    }

    std::vector<Equation> rows_;
    IndexSet pivot_col_idxs_;
    std::vector<std::size_t> col_idx_to_row_idx_;

    /// @brief Empty until the rows are a large enough fraction of the columns,
    /// since a table indexed by column costs a clause more than it saves it.
    std::vector<const V*> col_idx_to_row_;
    std::size_t cols_;
};

} // namespace xorcle
