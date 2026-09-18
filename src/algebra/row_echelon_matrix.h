#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/reduce_result.h"
#include "algebra/row_echelon_matrix_template.h"
#include "util/index_set.h"
#include "util/print_mode.h"

namespace xorcle {

/// @brief A matrix over GF(2) in row-echelon form.
///
/// Rows are guaranteed linearly independent. Wraps a
/// `RowEchelonMatrixInterface` pointer to allow value semantics.
class RowEchelonMatrix {
public:
    RowEchelonMatrix(EquationRepr repr, std::size_t cols);

    RowEchelonMatrix(const RowEchelonMatrix& other);
    RowEchelonMatrix(RowEchelonMatrix&& other) noexcept = default;
    RowEchelonMatrix& operator=(const RowEchelonMatrix& other);
    RowEchelonMatrix& operator=(RowEchelonMatrix&& other) noexcept = default;
    virtual ~RowEchelonMatrix() = default;

    EquationRepr repr() const noexcept { return interface_->repr(); }

    /// @brief Number of stored rows, i.e. the dimension of the span; a clause's width.
    std::size_t rows() const noexcept { return interface_->rows(); }

    std::size_t cols() const noexcept { return interface_->cols(); }

    /// @brief Equation at row `row_idx`, in row-echelon order.
    /// @pre `row_idx < rows()`.
    const Equation& row(std::size_t row_idx) const {
        assert(row_idx < rows() && "row index is out of range");
        return interface_->row(row_idx);
    }

    /// @brief The pivot columns, indexed by column, over `cols()` indices.
    const IndexSet& pivot_col_idxs() const noexcept { return interface_->pivot_col_idxs(); }

    /// @brief Does nothing if `v` is already in the span.
    /// @pre `v.num_bits() == cols()` and `v.repr() == repr()`, here and in
    /// `add_row_get_row_idx()`.
    void add_row(Equation v) { add_row_get_row_idx(std::move(v)); }

    /// @brief The inserted row_idx, or `std::nullopt` if `v` was already in the span.
    std::optional<std::size_t> add_row_get_row_idx(Equation v) {
        assert(v.num_bits() == cols() && "add_row: bit count does not match the matrix");
        assert(v.repr() == repr() && "add_row: representation does not match the matrix");
        return interface_->add_row_get_row_idx(std::move(v));
    }

    /// @brief Reduce `v` in place by every stored row whose pivot it carries.
    /// @pre `v.num_bits() == cols()` and `v.repr() == repr()`.
    void reduce(Equation& v) const {
        assert(v.num_bits() == cols() && "reduce: bit count does not match the matrix");
        assert(v.repr() == repr() && "reduce: representation does not match the matrix");
        interface_->reduce(v);
    }

    /// @brief Reduce `v` in place and report which rows took part.
    /// @return One bit per participating row, sized `rows()` and indexed by row
    /// index, not by column.
    /// @pre `v.num_bits() == cols()` and `v.repr() == repr()`.
    IndexSet reduce_get_coefficients(Equation& v) const {
        assert(v.num_bits() == cols() &&
               "reduce_get_coefficients: bit count does not match the matrix");
        assert(v.repr() == repr() &&
               "reduce_get_coefficients: representation does not match the matrix");
        return interface_->reduce_get_coefficients(v);
    }

    /// @brief Add `eqn` into the row at `row_idx`, in place.
    ///
    /// Changes the representative of the row, never the span or the echelon
    /// order: the row keeps its leading column and every pivot table stays valid.
    /// @pre `row_idx < rows()`, `eqn.num_bits() == cols()`, `eqn.repr() == repr()`.
    /// @pre `eqn`'s leading bit is strictly after the row's leading column.
    void add_to_row(std::size_t row_idx, const Equation& eqn) {
        assert(row_idx < rows() && "add_to_row index is out of range");
        assert(eqn.num_bits() == cols() && "add_to_row: bit count does not match the matrix");
        assert(eqn.repr() == repr() && "add_to_row: representation does not match the matrix");
        interface_->add_to_row(row_idx, eqn);
    }

    /// @brief Remove the row at `row_idx`, returning it.
    /// @pre `row_idx < rows()`.
    Equation pop_row(std::size_t row_idx) {
        assert(row_idx < rows() && "pop_row index is out of range");
        return interface_->pop_row(row_idx);
    }

    /// @pre `rows() > 0`.
    Equation pop_first_row() { return pop_row(0); }

    /// @pre `rows() > 0`.
    void pop_last_row() {
        assert(rows() > 0 && "pop_last_row on an empty matrix");
        interface_->pop_last_row();
    }

    class iterator {
    public:
        iterator(const RowEchelonMatrix* matrix, std::size_t row_idx) noexcept
            : matrix_(matrix), row_idx_(row_idx) {}

        iterator& operator++() noexcept {
            ++row_idx_;
            return *this;
        }

        const Equation& operator*() const { return matrix_->row(row_idx_); }

        bool operator==(const iterator& other) const noexcept = default;

    private:
        const RowEchelonMatrix* matrix_;
        std::size_t row_idx_;
    };

    iterator begin() const noexcept { return iterator(this, 0); }

    iterator end() const noexcept { return iterator(this, rows()); }

protected:
    /// @brief For use inside `src/algebra/` only.
    RowEchelonMatrixInterface& interface() const noexcept { return *interface_; }

private:
    std::unique_ptr<RowEchelonMatrixInterface> interface_;
};

/// @brief A subspace of the space of linear equations over GF(2), whose last
/// column is the constant.
class RowSpace : public RowEchelonMatrix {
public:
    RowSpace(EquationRepr repr, std::size_t num_vars) : RowEchelonMatrix(repr, num_vars + 1) {}

    std::size_t num_vars() const noexcept { return cols() - 1; }

    /// @brief Reduce `v` in place only until its leading column is not a pivot.
    /// @pre `v.num_bits() == cols()` and `v.repr() == repr()`.
    ReduceResult partial_reduce(Equation& v) const {
        assert(v.num_bits() == cols() && "partial_reduce: bit count does not match the matrix");
        assert(v.repr() == repr() &&
               "partial_reduce: representation does not match the matrix");
        return interface().partial_reduce(v);
    }

    /// @brief Partially reduce `v` against this matrix augmented with `extra`.
    /// @pre `v` and `extra` each have `cols()` bits and this matrix's representation.
    /// @pre `extra`'s leading column is not a pivot of this matrix, or `extra` is `0 = 0`.
    ReduceResult partial_reduce_with_extra(Equation& v, const Equation& extra) const {
        assert(v.num_bits() == cols() && extra.num_bits() == cols() &&
               "partial_reduce_with_extra: bit count does not match the matrix");
        assert(v.repr() == repr() && extra.repr() == repr() &&
               "partial_reduce_with_extra: representation does not match the matrix");
        assert((!extra.leading_bit() || !pivot_col_idxs().test(*extra.leading_bit())) &&
               "extra must already be partially reduced by this matrix");
        return interface().partial_reduce_with_extra(v, extra);
    }

    /// @brief The unique satisfying assignment, indexed by variable.
    /// @pre `rows() + 1 == cols()` and the false equation is not in the matrix.
    IndexSet unique_solution() const;
};

/// @brief A clause stored as its linear negation.
///
/// The linear negation of `f1 v ... v fk` is the span of the `fi + [0 = 1]`, so
/// `[x1 + x2 = 1] v [x3 = 0]` is stored as `[x1 + x2 = 0], [x3 = 1]`.
class Clause : public RowSpace {
public:
    using RowSpace::RowSpace;

    std::size_t watched0 = 0; ///< Index of the first watched equation.
    std::size_t watched1 = 1; ///< Index of the second watched equation.

    /// @brief Where the next cyclic search for a replacement watch starts.
    std::size_t next_watch_search_start = 0;

    std::string to_string(const PrintMode& mode = {}) const;
};

std::unique_ptr<RowEchelonMatrixInterface> make_row_echelon_matrix_interface(
    EquationRepr repr, std::size_t cols);

} // namespace xorcle
