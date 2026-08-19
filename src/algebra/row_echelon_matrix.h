#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/reduce_result.h"
#include "util/bitmap.h"
#include "util/print_mode.h"

namespace xorcle {

/// @brief The abstract interface behind `RowEchelonMatrix`.
///
/// Every method is a whole-row or whole-loop operation, so the reduction loops
/// pay one virtual call in total rather than one per row operation or per bit.
/// Each representation derives from this through
/// `RowEchelonMatrixTemplate<GF2Vector>`; nothing outside `src/algebra/` should
/// name this type. The operations are documented on `RowEchelonMatrix`, which
/// forwards.
class RowEchelonMatrixInterface {
public:
    virtual ~RowEchelonMatrixInterface() = default;

    virtual EquationRepr repr() const noexcept = 0;

    virtual std::unique_ptr<RowEchelonMatrixInterface> clone() const = 0;

    virtual std::size_t rows() const noexcept = 0;

    virtual std::size_t num_vars() const noexcept = 0;

    virtual const Equation& row(std::size_t index) const = 0;

    virtual const Bitmap& pivot_columns() const noexcept = 0;

    virtual std::optional<std::size_t> add_row_get_index(const Equation& v) = 0;

    virtual void reduce(Equation& v) const = 0;

    virtual void reduce(Equation& v, Bitmap& coefficients_out) const = 0;

    virtual ReduceResult partial_reduce(Equation& v) const = 0;

    virtual ReduceResult partial_reduce_with_extra(Equation& v, const Equation& extra) const = 0;

    virtual Equation pop_first_row() = 0;

    virtual void pop_last_row() = 0;

    /// @brief Write the unique solution of the stored system into `out`.
    virtual void solution(Bitmap& out) const = 0;
};

/// @brief A matrix over GF(2) in row-echelon form.
///
/// Representation invariants:
/// - `pivot_columns().size() == num_vars() + 1`
/// - every stored row has width `cols()`
/// - stored rows have strictly increasing leading columns
/// - `pivot_columns()` marks exactly the leading column of each stored row
///
/// Reading out a whole column is deliberately absent: it is the one operation
/// genuinely awful for the sparse representation, and every algorithm wanting it
/// can be written row-wise instead. Every `Equation` argument below must satisfy
/// `size() == cols()` and `repr() == repr()`.
class RowEchelonMatrix {
public:
    RowEchelonMatrix(EquationRepr repr, std::size_t num_vars);

    RowEchelonMatrix(const RowEchelonMatrix& other);
    RowEchelonMatrix(RowEchelonMatrix&& other) noexcept = default;
    RowEchelonMatrix& operator=(const RowEchelonMatrix& other);
    RowEchelonMatrix& operator=(RowEchelonMatrix&& other) noexcept = default;
    virtual ~RowEchelonMatrix() = default;

    EquationRepr repr() const noexcept { return storage_->repr(); }

    /// @brief Number of stored rows, i.e. the rank.
    std::size_t rows() const noexcept { return storage_->rows(); }

    /// @brief Number of columns, i.e. `num_vars() + 1`.
    std::size_t cols() const noexcept { return storage_->num_vars() + 1; }

    std::size_t num_vars() const noexcept { return storage_->num_vars(); }

    /// @brief Row `index`, in row-echelon order.
    const Equation& row(std::size_t index) const { return storage_->row(index); }

    /// @brief Bitmap of the pivot columns, of width `cols()`.
    const Bitmap& pivot_columns() const noexcept { return storage_->pivot_columns(); }

    /// @brief Does nothing if `v` is already in the span.
    void add_row(const Equation& v) { storage_->add_row_get_index(v); }

    /// @brief The inserted index, or `std::nullopt` if `v` was already in the span.
    std::optional<std::size_t> add_row_get_index(const Equation& v) {
        return storage_->add_row_get_index(v);
    }

    void reduce(Equation& v) const { storage_->reduce(v); }

    /// @brief `coefficients_out` gets one bit per participating row, sized `rows()`.
    void reduce(Equation& v, Bitmap& coefficients_out) const {
        storage_->reduce(v, coefficients_out);
    }

    /// @brief Reduce `v` in place only until its leading column is not a pivot.
    ReduceResult partial_reduce(Equation& v) const { return storage_->partial_reduce(v); }

    /// @brief Partially reduce `v` against this matrix augmented with `extra`.
    /// @pre `extra` is @f$\mathbf{0}@f$, or its leading column is not a pivot of this matrix.
    ReduceResult partial_reduce_with_extra(Equation& v, const Equation& extra) const {
        return storage_->partial_reduce_with_extra(v, extra);
    }

    /// @pre `rows() > 0`.
    Equation pop_first_row() { return storage_->pop_first_row(); }

    /// @pre `rows() > 0`.
    void pop_last_row() { storage_->pop_last_row(); }

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
    RowEchelonMatrixInterface& storage() const noexcept { return *storage_; }

private:
    std::unique_ptr<RowEchelonMatrixInterface> storage_;
};

/// @brief A subspace of the space of linear equations over GF(2), held as a
/// basis in row-echelon form. Each column is a variable in the original order.
class RowSpace : public RowEchelonMatrix {
public:
    using RowEchelonMatrix::RowEchelonMatrix;

    /// @brief One bit per variable.
    /// @pre The row space has exactly one solution; equivalently, every variable
    /// column is pivoted and the constant column is not.
    Bitmap unique_solution() const;
};

/// @brief A clause in its linear negation: the conjunction of the linear
/// equations whose common solutions are exactly where the clause is false.
/// A clause @f$\bigvee_i f_i@f$ is stored as @f$\mathrm{span}(f_i +
/// \mathbf{1})@f$, so `[x1 + x2 = 1] v [x3 = 0]` is stored as
/// `[x1 + x2 = 0] ^ [x3 = 1]`, i.e. `[[1, 1, 0, 0], [0, 0, 1, 1]]`.
class Clause : public RowSpace {
public:
    using RowSpace::RowSpace;

    std::size_t watched0 = 0; ///< Index of the first watched equation.
    std::size_t watched1 = 1; ///< Index of the second watched equation.

    /// @brief Where the next cyclic search for a replacement watch starts.
    std::size_t next_watch_search_start = 0;

    std::string to_string(const PrintMode& mode = {}) const;
};

// The two factories below add the constant column, so widths are `num_vars + 1`.

std::unique_ptr<RowEchelonMatrixInterface> make_row_echelon_matrix_interface(
    EquationRepr repr, std::size_t num_vars);

RowEchelonMatrix make_row_echelon_matrix(EquationRepr repr, std::size_t num_vars);

} // namespace xorcle
