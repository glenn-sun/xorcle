#pragma once

#ifndef XORCLE_COL_IDX_TO_ROW_DIVISOR
#define XORCLE_COL_IDX_TO_ROW_DIVISOR 8
#endif

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
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

    virtual std::size_t pivot_col_idx(std::size_t row_idx) const noexcept = 0;

    virtual std::optional<std::size_t> add_row_get_row_idx(Equation v) = 0;

    virtual void reduce(Equation& v) const = 0;

    virtual IndexSet reduce_get_coefficients(Equation& v) const = 0;

    /// @brief Reduce `v` in place and record the participating rows in
    /// `coefficients`, resized to `rows()` and emptied first.
    virtual void reduce_get_coefficients(Equation& v, IndexSet& coefficients) const = 0;

    /// @brief Reduce `v` in place only until its leading column is not a pivot,
    /// reading the last column as the constant.
    virtual ReduceResult partial_reduce(Equation& v) const = 0;

    /// @brief `partial_reduce(v)` for a `v` known to lead in `leading_col_idx`.
    virtual ReduceResult partial_reduce(Equation& v, std::size_t leading_col_idx) const = 0;

    /// @brief Partially reduce `v` against this matrix augmented with `extra`,
    /// reading the last column as the constant.
    virtual ReduceResult partial_reduce_with_extra(Equation& v, const Equation& extra) const = 0;

    /// @brief `partial_reduce_with_extra(v, extra)` for a `v` known to lead in
    /// `leading_col_idx` and an `extra` known to lead in `extra_leading_col_idx`.
    virtual ReduceResult partial_reduce_with_extra(
        Equation& v, std::size_t leading_col_idx, const Equation& extra,
        std::optional<std::size_t> extra_leading_col_idx) const = 0;

    /// @brief Partially reduce `v` by the rows whose `row_idx_to_order` entry is
    /// below `bound`, reading the last column as the constant.
    virtual ReduceResult partial_reduce_below(Equation& v,
                                              std::span<const std::size_t> row_idx_to_order,
                                              std::size_t bound) const = 0;

    /// @brief Add `eqn` into the row at `row_idx`, which must keep its pivot.
    virtual void add_to_row(std::size_t row_idx, const Equation& eqn) = 0;

    /// @brief Clear every pivot column in every row but the one it leads.
    virtual void reduce_rows() = 0;

    /// @brief Remove the row at `row_idx`, returning it.
    virtual Equation pop_row(std::size_t row_idx) = 0;

    /// @brief Remove every row whose entry in `is_popped` is nonzero.
    virtual void pop_rows(std::span<const std::uint8_t> is_popped) = 0;

    virtual void pop_last_row() = 0;

    /// @brief The columns some row carries.
    virtual IndexSet carried_col_idxs() const = 0;

    virtual bool carries(std::size_t col_idx) const = 0;

    /// @brief Renumber every row in place, reading the last column as the constant.
    virtual void compact(const CompactionTransform& transform) = 0;

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

    // `col_idx_to_row_` points into another matrix's rows, so a copy has to rebuild it.
    RowEchelonMatrixTemplate(const RowEchelonMatrixTemplate& other)
        : rows_(other.rows_), pivot_col_idxs_(other.pivot_col_idxs_),
          cols_(other.cols_) {
        if (!other.col_idx_to_row_.empty()) {
            build_col_idx_to_row();
        }
    }

    RowEchelonMatrixTemplate& operator=(const RowEchelonMatrixTemplate& other) {
        if (this != &other) {
            rows_ = other.rows_;
            pivot_col_idxs_ = other.pivot_col_idxs_;
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

    const Equation& row(std::size_t row_idx) const override { return rows_[row_idx].row; }

    const IndexSet& pivot_col_idxs() const noexcept override { return pivot_col_idxs_; }

    std::size_t pivot_col_idx(std::size_t row_idx) const noexcept override {
        return rows_[row_idx].pivot_col_idx;
    }

    std::optional<std::size_t> add_row_get_row_idx(Equation v) override {
        // `v` is the caller's own copy, so it is reduced in place and stored.
        V& reduced_row = gf2_vector_of<V>(v);
        std::optional<std::size_t> leading_col_idx = reduced_row.leading_bit();
        while (leading_col_idx && pivot_col_idxs_.test(*leading_col_idx)) {
            leading_col_idx = reduced_row.plus_assign_and_next_bit(
                gf2_vector_of<V>(rows_[row_idx_for_col_idx(*leading_col_idx)].row),
                *leading_col_idx);
        }
        if (!leading_col_idx) {
            return std::nullopt; // `v` already belongs to the span
        }

        // Row-echelon order means the insertion row_idx is the number of pivots
        // strictly below the new leading column
        const std::size_t new_pivot_col_idx = *leading_col_idx;
        const std::size_t insert_row_idx = pivot_col_idxs_.count_below(new_pivot_col_idx);

        rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(insert_row_idx),
                     StoredRow{std::move(v), new_pivot_col_idx});
        pivot_col_idxs_.set(new_pivot_col_idx);
        // Inserting moves handles, never the equations they own, so the entries
        // for the other pivot columns stay valid.
        if (!col_idx_to_row_.empty()) {
            col_idx_to_row_[new_pivot_col_idx] = &gf2_vector_of<V>(rows_[insert_row_idx].row);
        } else if (rows_.size() * col_idx_to_row_divisor >= cols_) {
            build_col_idx_to_row();
        }

        debug_assert_valid_representation();
        return insert_row_idx;
    }

    void reduce(Equation& v) const override {
        V& vec = gf2_vector_of<V>(v);
        // `v` is mutated while its set pivot columns are being walked, so each
        // step reads the live vector.
        for (auto col_idx = vec.first_bit_in(pivot_col_idxs_, 0); col_idx;
             col_idx = vec.first_bit_in(pivot_col_idxs_, *col_idx + 1)) {
            vec.plus_assign(gf2_vector_of<V>(rows_[row_idx_for_col_idx(*col_idx)].row));
        }
    }

    IndexSet reduce_get_coefficients(Equation& v) const override {
        IndexSet coefficients;
        reduce_get_coefficients(v, coefficients);
        return coefficients;
    }

    void reduce_get_coefficients(Equation& v, IndexSet& coefficients) const override {
        coefficients.resize(rows_.size());
        V& vec = gf2_vector_of<V>(v);
        for (auto col_idx = vec.first_bit_in(pivot_col_idxs_, 0); col_idx;
             col_idx = vec.first_bit_in(pivot_col_idxs_, *col_idx + 1)) {
            const std::size_t row_idx = row_idx_for_col_idx(*col_idx);
            vec.plus_assign(gf2_vector_of<V>(rows_[row_idx].row));
            coefficients.set(row_idx);
        }
    }

    ReduceResult partial_reduce(Equation& v) const override {
        const std::optional<std::size_t> leading_col_idx = gf2_vector_of<V>(v).leading_bit();
        return col_idx_to_row_.empty() ? partial_reduce_impl<false>(v, leading_col_idx)
                                       : partial_reduce_impl<true>(v, leading_col_idx);
    }

    ReduceResult partial_reduce(Equation& v, std::size_t leading_col_idx) const override {
        return col_idx_to_row_.empty() ? partial_reduce_impl<false>(v, leading_col_idx)
                                       : partial_reduce_impl<true>(v, leading_col_idx);
    }

    ReduceResult partial_reduce_with_extra(Equation& v, const Equation& extra) const override {
        const std::optional<std::size_t> leading_col_idx = gf2_vector_of<V>(v).leading_bit();
        const std::optional<std::size_t> extra_leading_col_idx =
            gf2_vector_of<V>(extra).leading_bit();
        return col_idx_to_row_.empty()
                   ? partial_reduce_with_extra_impl<false>(v, leading_col_idx, extra,
                                                           extra_leading_col_idx)
                   : partial_reduce_with_extra_impl<true>(v, leading_col_idx, extra,
                                                          extra_leading_col_idx);
    }

    ReduceResult partial_reduce_with_extra(
        Equation& v, std::size_t leading_col_idx, const Equation& extra,
        std::optional<std::size_t> extra_leading_col_idx) const override {
        return col_idx_to_row_.empty()
                   ? partial_reduce_with_extra_impl<false>(v, leading_col_idx, extra,
                                                           extra_leading_col_idx)
                   : partial_reduce_with_extra_impl<true>(v, leading_col_idx, extra,
                                                          extra_leading_col_idx);
    }

    ReduceResult partial_reduce_below(Equation& v, std::span<const std::size_t> row_idx_to_order,
                                      std::size_t bound) const override {
        // The order table is indexed by row, so `col_idx_to_row_` saves nothing.
        V& vec = gf2_vector_of<V>(v);
        std::optional<std::size_t> col_idx = vec.leading_bit();
        while (true) {
            if (!col_idx) {
                return ReduceResult::eq_true();
            }
            if (!pivot_col_idxs_.test(*col_idx)) {
                return classify_col_idx(*col_idx);
            }
            const std::size_t row_idx = row_idx_for_col_idx(*col_idx);
            if (row_idx_to_order[row_idx] >= bound) {
                return classify_col_idx(*col_idx);
            }
            col_idx = vec.plus_assign_and_next_bit(gf2_vector_of<V>(rows_[row_idx].row), *col_idx);
        }
    }

    void add_to_row(std::size_t row_idx, const Equation& eqn) override {
        // Leading strictly later leaves every pivot, and so both tables, alone.
        gf2_vector_of<V>(rows_[row_idx].row).plus_assign(gf2_vector_of<V>(eqn));

        debug_assert_valid_representation();
    }

    void reduce_rows() override {
        // From the last row up, so the rows added in are already reduced and
        // each pivot column costs one addition.
        for (std::size_t row_idx = rows_.size(); row_idx-- > 0;) {
            V& row = gf2_vector_of<V>(rows_[row_idx].row);
            for (auto col_idx = row.next_bit(rows_[row_idx].pivot_col_idx); col_idx;
                 col_idx = row.next_bit(*col_idx)) {
                if (pivot_col_idxs_.test(*col_idx)) {
                    row.plus_assign(col_idx_to_row_.empty() ? row_for_col_idx<false>(*col_idx)
                                                            : row_for_col_idx<true>(*col_idx));
                }
            }
        }
        debug_assert_valid_representation();
    }

    Equation pop_row(std::size_t row_idx) override {
        Equation popped_row = std::move(rows_[row_idx].row);
        const std::optional<std::size_t> leading_col_idx =
            gf2_vector_of<V>(popped_row).leading_bit();
        // A structural invariant: a stored row is never zero.
        assert(leading_col_idx && "stored row has no pivot");

        rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(row_idx));
        pivot_col_idxs_.reset(*leading_col_idx);
        if (!col_idx_to_row_.empty()) {
            col_idx_to_row_[*leading_col_idx] = nullptr;
        }

        debug_assert_valid_representation();
        return popped_row;
    }

    void pop_rows(std::span<const std::uint8_t> is_popped) override {
        // Moving handles never moves the equations they own, so the entries for
        // the kept pivot columns stay valid.
        std::size_t kept = 0;
        for (std::size_t row_idx = 0; row_idx < rows_.size(); ++row_idx) {
            if (is_popped[row_idx] != 0) {
                pivot_col_idxs_.reset(rows_[row_idx].pivot_col_idx);
                if (!col_idx_to_row_.empty()) {
                    col_idx_to_row_[rows_[row_idx].pivot_col_idx] = nullptr;
                }
            } else {
                if (kept != row_idx) {
                    rows_[kept] = std::move(rows_[row_idx]);
                }
                ++kept;
            }
        }
        rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(kept), rows_.end());

        debug_assert_valid_representation();
    }

    void pop_last_row() override {
        const std::optional<std::size_t> leading_col_idx =
            gf2_vector_of<V>(rows_.back().row).leading_bit();
        // A structural invariant: a stored row is never zero.
        assert(leading_col_idx && "stored row has no pivot");

        rows_.pop_back();
        pivot_col_idxs_.reset(*leading_col_idx);
        if (!col_idx_to_row_.empty()) {
            col_idx_to_row_[*leading_col_idx] = nullptr;
        }

        debug_assert_valid_representation();
    }

    IndexSet carried_col_idxs() const override {
        IndexSet carried(cols_);
        for (const StoredRow& stored : rows_) {
            gf2_vector_of<V>(stored.row).add_set_bits_to(carried);
        }
        return carried;
    }

    bool carries(std::size_t col_idx) const override {
        return std::ranges::any_of(rows_, [col_idx](const StoredRow& stored) {
            return gf2_vector_of<V>(stored.row).get_bit(col_idx);
        });
    }

    void compact(const CompactionTransform& transform) override {
        const std::size_t old_constant_col_idx = transform.old_num_vars();
        const std::size_t new_constant_col_idx = transform.new_num_vars();
        const auto to_new = [&](std::size_t col_idx) {
            return col_idx == old_constant_col_idx ? new_constant_col_idx
                                                   : transform.to_new(col_idx);
        };
        cols_ = new_constant_col_idx + 1;
        pivot_col_idxs_.clear();
        pivot_col_idxs_.resize(cols_);
        for (StoredRow& stored : rows_) {
            V& row = gf2_vector_of<V>(stored.row);
            // No column moves up and the survivors keep their order, so moving the
            // set bits in ascending order never lands on one still to be moved.
            for (auto col_idx = row.leading_bit(); col_idx; col_idx = row.next_bit(*col_idx)) {
                const std::size_t new_col_idx = to_new(*col_idx);
                if (new_col_idx == *col_idx) {
                    continue;
                }
                row.reset_bit(*col_idx);
                if (new_col_idx != CompactionTransform::npos) {
                    row.set_bit(new_col_idx);
                }
            }
            row.resize(cols_);
            stored.pivot_col_idx = to_new(stored.pivot_col_idx);
            pivot_col_idxs_.set(stored.pivot_col_idx);
        }
        col_idx_to_row_.clear();
        if (rows_.size() * col_idx_to_row_divisor >= cols_) {
            build_col_idx_to_row();
        }

        debug_assert_valid_representation();
    }

    IndexSet solution() const override {
        const std::size_t constant_col_idx = cols_ - 1;
        IndexSet out(constant_col_idx);
        for (std::size_t i = constant_col_idx; i-- > 0;) {
            const V& row = gf2_vector_of<V>(rows_[i].row);
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
            return gf2_vector_of<V>(rows_[row_idx_for_col_idx(col_idx)].row);
        }
    }

    template <bool FromColIdxToRow>
    ReduceResult partial_reduce_impl(Equation& v, std::optional<std::size_t> col_idx) const {
        V& vec = gf2_vector_of<V>(v);
        while (true) {
            if (!col_idx) {
                return ReduceResult::eq_true();
            }
            if (!pivot_col_idxs_.test(*col_idx)) {
                return classify_col_idx(*col_idx);
            }
            col_idx = vec.plus_assign_and_next_bit(row_for_col_idx<FromColIdxToRow>(*col_idx),
                                                   *col_idx);
        }
    }

    template <bool FromColIdxToRow>
    ReduceResult partial_reduce_with_extra_impl(
        Equation& v, std::optional<std::size_t> col_idx, const Equation& extra,
        std::optional<std::size_t> extra_leading_col_idx) const {
        const V& extra_vec = gf2_vector_of<V>(extra);
        V& vec = gf2_vector_of<V>(v);
        while (true) {
            if (!col_idx) {
                return ReduceResult::eq_true();
            }
            if (!pivot_col_idxs_.test(*col_idx)) {
                if (extra_leading_col_idx && *col_idx == *extra_leading_col_idx) {
                    col_idx = vec.plus_assign_and_next_bit(extra_vec, *col_idx);
                    continue;
                }
                return classify_col_idx(*col_idx);
            }
            col_idx = vec.plus_assign_and_next_bit(row_for_col_idx<FromColIdxToRow>(*col_idx),
                                                   *col_idx);
        }
    }

    /// @brief The row holding the pivot in `col_idx`.
    std::size_t row_idx_for_col_idx(std::size_t col_idx) const noexcept {
        return pivot_col_idxs_.count_below(col_idx);
    }

    void build_col_idx_to_row() {
        col_idx_to_row_.assign(cols_, nullptr);
        // `for_each_set` walks ascending, which is row-echelon order.
        std::size_t row_idx = 0;
        pivot_col_idxs_.for_each_set([&](std::size_t pivot_col_idx) {
            col_idx_to_row_[pivot_col_idx] = &gf2_vector_of<V>(rows_[row_idx++].row);
        });
    }

    ReduceResult classify_col_idx(std::size_t col_idx) const noexcept {
        return col_idx + 1 == cols_ ? ReduceResult::eq_false() : ReduceResult::leading(col_idx);
    }

    /// @brief Check representation invariants for `RowEchelonMatrix`.
    void debug_assert_valid_representation() const {
#ifndef NDEBUG
        assert(pivot_col_idxs_.size() == cols() && "the pivot set is not sized by column");
        assert(pivot_col_idxs_.count() == rows_.size() && "pivot count differs from row count");

        std::optional<std::size_t> previous_pivot_col_idx;
        for (std::size_t row_idx = 0; row_idx < rows_.size(); ++row_idx) {
            const V& current = gf2_vector_of<V>(rows_[row_idx].row);
            assert(current.num_bits() == cols() && "a row's bit count differs from cols()");

            const std::optional<std::size_t> leading_col_idx = current.leading_bit();
            assert(leading_col_idx && *leading_col_idx < cols() && "a stored row is zero");
            if (previous_pivot_col_idx) {
                assert(*previous_pivot_col_idx < *leading_col_idx &&
                       "rows are not in row-echelon order");
            }
            assert(pivot_col_idxs_.test(*leading_col_idx) &&
                   "a row's leading column is not in the pivot set");
            assert(row_idx_for_col_idx(*leading_col_idx) == row_idx &&
                   "a pivot column maps to the wrong row");
            assert(rows_[row_idx].pivot_col_idx == *leading_col_idx &&
                   "a row's cached pivot column is stale");
            previous_pivot_col_idx = leading_col_idx;
        }

        pivot_col_idxs_.for_each_set([&](std::size_t pivot_col_idx) {
            assert(row_idx_for_col_idx(pivot_col_idx) < rows_.size() &&
                   "a pivot column maps past the last row");
            assert(gf2_vector_of<V>(rows_[row_idx_for_col_idx(pivot_col_idx)].row)
                           .leading_bit() == std::optional<std::size_t>(pivot_col_idx) &&
                   "a pivot column's row does not lead on it");
            assert((col_idx_to_row_.empty() ||
                    col_idx_to_row_[pivot_col_idx] ==
                        &gf2_vector_of<V>(rows_[row_idx_for_col_idx(pivot_col_idx)].row)) &&
                   "col_idx_to_row_ is stale");
        });
#endif
    }

    struct StoredRow {
        Equation row;
        std::size_t pivot_col_idx;
    };

    std::vector<StoredRow> rows_;
    IndexSet pivot_col_idxs_;

    /// @brief Empty until the rows are a large enough fraction of the columns.
    std::vector<const V*> col_idx_to_row_;
    std::size_t cols_;
};

} // namespace xorcle
