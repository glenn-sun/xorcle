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

/// @brief A matrix over GF(2) in row-echelon form. Rows are guaranteed
/// linearly independent. Wraps a `RowEchelonMatrixInterface` pointer
/// to allow value semantics.
class RowEchelonMatrix {
public:
    RowEchelonMatrix(EquationRepr repr, std::size_t num_vars);

    RowEchelonMatrix(const RowEchelonMatrix& other);
    RowEchelonMatrix(RowEchelonMatrix&& other) noexcept = default;
    RowEchelonMatrix& operator=(const RowEchelonMatrix& other);
    RowEchelonMatrix& operator=(RowEchelonMatrix&& other) noexcept = default;
    virtual ~RowEchelonMatrix() = default;

    EquationRepr repr() const noexcept { return interface_->repr(); }

    /// @brief Number of stored rows, i.e. the rank.
    std::size_t rows() const noexcept { return interface_->rows(); }

    /// @brief Number of columns, i.e. `num_vars() + 1`.
    std::size_t cols() const noexcept { return interface_->num_vars() + 1; }

    std::size_t num_vars() const noexcept { return interface_->num_vars(); }

    /// @brief Equation at row `index`, in row-echelon order.
    /// @pre `index < rows()`.
    const Equation& row(std::size_t index) const {
        assert(index < rows() && "row index is out of range");
        return interface_->row(index);
    }

    /// @brief The pivot columns, indexed by column, over `cols()` indices.
    const IndexSet& pivot_columns() const noexcept { return interface_->pivot_columns(); }

    /// @brief Does nothing if `v` is already in the span.
    /// @pre `v.num_bits() == cols()` and `v.repr() == repr()`.
    void add_row(const Equation& v) { interface_->add_row_get_index(v); }

    /// @brief The inserted index, or `std::nullopt` if `v` was already in the span.
    /// @pre `v.num_bits() == cols()` and `v.repr() == repr()`.
    std::optional<std::size_t> add_row_get_index(const Equation& v) {
        return interface_->add_row_get_index(v);
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

    /// @brief Reduce `v` in place only until its leading column is not a pivot.
    /// @pre `v.num_bits() == cols()` and `v.repr() == repr()`.
    ReduceResult partial_reduce(Equation& v) const {
        assert(v.num_bits() == cols() && "partial_reduce: bit count does not match the matrix");
        assert(v.repr() == repr() &&
               "partial_reduce: representation does not match the matrix");
        return interface_->partial_reduce(v);
    }

    /// @brief Partially reduce `v` against this matrix augmented with `extra`.
    /// @pre `v` and `extra` each have `cols()` bits and this matrix's representation.
    /// @pre `extra`'s leading column is not a pivot of this matrix, or `extra` is `0 = 0`.
    ReduceResult partial_reduce_with_extra(Equation& v, const Equation& extra) const {
        assert(v.num_bits() == cols() && extra.num_bits() == cols() &&
               "partial_reduce_with_extra: bit count does not match the matrix");
        assert(v.repr() == repr() && extra.repr() == repr() &&
               "partial_reduce_with_extra: representation does not match the matrix");
        assert((!extra.leading_bit() || !pivot_columns().test(*extra.leading_bit())) &&
               "extra must already be partially reduced by this matrix");
        return interface_->partial_reduce_with_extra(v, extra);
    }

    /// @pre `rows() > 0`.
    Equation pop_first_row() {
        assert(rows() > 0 && "pop_first_row on an empty matrix");
        return interface_->pop_first_row();
    }

    /// @pre `rows() > 0`.
    void pop_last_row() {
        assert(rows() > 0 && "pop_last_row on an empty matrix");
        interface_->pop_last_row();
    }

    class iterator {
    public:
        iterator(const RowEchelonMatrix* matrix, std::size_t index) noexcept
            : matrix_(matrix), index_(index) {}

        iterator& operator++() noexcept {
            ++index_;
            return *this;
        }

        const Equation& operator*() const { return matrix_->row(index_); }

        bool operator==(const iterator& other) const noexcept = default;

    private:
        const RowEchelonMatrix* matrix_;
        std::size_t index_;
    };

    iterator begin() const noexcept { return iterator(this, 0); }

    iterator end() const noexcept { return iterator(this, rows()); }

protected:
    /// @brief Access the underlying interface. For use inside `src/algebra/` only.
    RowEchelonMatrixInterface& interface() const noexcept { return *interface_; }

private:
    std::unique_ptr<RowEchelonMatrixInterface> interface_;
};

/// @brief Represents a subspace of the space of linear equations over GF(2).
/// Contrast with TrailBasisSpace, which is also backed by a matrix but has a
/// change of basis applied.
class RowSpace : public RowEchelonMatrix {
public:
    using RowEchelonMatrix::RowEchelonMatrix;

    /// @brief The unique satisfying assignment, indexed by variable.
    /// @pre `rows() + 1 == cols()` and the false equation is not in the matrix.
    IndexSet unique_solution() const;
};

/// @brief A clause in its linear negation: the conjunction of the linear
/// equations whose common solutions are exactly where the clause is false.
/// A clause @f$\bigvee_i f_i@f$ is stored as @f$\mathrm{span}(f_i +
/// \mathbf{1})@f$, so `[x1 + x2 = 1] v [x3 = 0]` is stored as
/// `[x1 + x2 = 0] ^ [x3 = 1]`.
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
    EquationRepr repr, std::size_t num_vars);

RowEchelonMatrix make_row_echelon_matrix(EquationRepr repr, std::size_t num_vars);

} // namespace xorcle
