#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/reduce_result.h"
#include "util/index_set.h"
#include "util/print_mode.h"

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

    virtual std::size_t num_vars() const noexcept = 0;

    virtual const Equation& row(std::size_t index) const = 0;

    virtual const IndexSet& pivot_columns() const noexcept = 0;

    virtual std::optional<std::size_t> add_row_get_index(const Equation& v) = 0;

    virtual void reduce(Equation& v) const = 0;

    virtual void reduce(Equation& v, IndexSet& coefficients_out) const = 0;

    virtual ReduceResult partial_reduce(Equation& v) const = 0;

    virtual ReduceResult partial_reduce_with_extra(Equation& v, const Equation& extra) const = 0;

    virtual Equation pop_first_row() = 0;

    virtual void pop_last_row() = 0;

    /// @brief The unique solution of the stored system, one bit per variable.
    virtual IndexSet solution() const = 0;
};

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
    const Equation& row(std::size_t index) const { return interface_->row(index); }

    /// @brief The pivot columns, indexed by column, over `cols()` indices.
    const IndexSet& pivot_columns() const noexcept { return interface_->pivot_columns(); }

    /// @brief Does nothing if `v` is already in the span.
    void add_row(const Equation& v) { interface_->add_row_get_index(v); }

    /// @brief The inserted index, or `std::nullopt` if `v` was already in the span.
    std::optional<std::size_t> add_row_get_index(const Equation& v) {
        return interface_->add_row_get_index(v);
    }

    void reduce(Equation& v) const { interface_->reduce(v); }

    /// @brief `coefficients_out` gets one bit per participating row, sized
    /// `rows()`. It is indexed by row index, not by column.
    ///
    /// @note An out-parameter rather than a return value because its only caller,
    /// `TrailBasisSpace::from_RowSpace()`, refills one `IndexSet` once per row of
    /// the row space it converts, from a buffer hoisted out of that loop.
    void reduce(Equation& v, IndexSet& coefficients_out) const {
        interface_->reduce(v, coefficients_out);
    }

    /// @brief Reduce `v` in place only until its leading column is not a pivot.
    ReduceResult partial_reduce(Equation& v) const { return interface_->partial_reduce(v); }

    /// @brief Partially reduce `v` against this matrix augmented with `extra`.
    /// @pre `extra`'s leading column is not a pivot of this matrix, or `extra` is `0 = 0`.
    ReduceResult partial_reduce_with_extra(Equation& v, const Equation& extra) const {
        return interface_->partial_reduce_with_extra(v, extra);
    }

    /// @pre `rows() > 0`.
    Equation pop_first_row() { return interface_->pop_first_row(); }

    /// @pre `rows() > 0`.
    void pop_last_row() { interface_->pop_last_row(); }

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
