#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "algebra/algebra.h"
#include "algebra/equation.h"
#include "algebra/reduce_result.h"
#include "util/bitmap.h"
#include "util/print_mode.h"

namespace xorcle {

/// @brief The abstract backend interface behind `RowEchelonMatrix`.
///
/// Every method is a whole-row or whole-loop operation, so the reduction loops
/// pay one virtual call in total rather than one per row operation or per bit.
/// Backends derive from this through the templated `RowEchelonMatrixImpl<Row>`;
/// nothing outside `src/algebra/` should name this type.
class RowEchelonMatrixStorage {
public:
    virtual ~RowEchelonMatrixStorage() = default;

    /// @brief Which backend implements this storage.
    virtual Algebra backend() const noexcept = 0;

    /// @brief Deep-copy this storage.
    virtual std::unique_ptr<RowEchelonMatrixStorage> clone() const = 0;

    /// @brief Number of stored rows.
    virtual std::size_t rows() const noexcept = 0;

    /// @brief Number of variable columns.
    virtual std::size_t num_vars() const noexcept = 0;

    /// @brief Row `index`, in row-echelon order.
    virtual const Equation& row(std::size_t index) const = 0;

    /// @brief Bitmap of the pivot columns, of width `num_vars() + 1`.
    virtual const Bitmap& pivot_columns() const noexcept = 0;

    /// @brief Index of the row whose leading column is `column`.
    virtual std::size_t row_for_pivot_column(std::size_t column) const = 0;

    /// @brief Insert a row, returning its index if it was not already in the span.
    virtual std::optional<std::size_t> add_row_get_index(const Equation& v) = 0;

    /// @brief Fully reduce `v` against this matrix.
    virtual void reduce(Equation& v) const = 0;

    /// @brief Fully reduce `v`, recording which rows participated.
    virtual void reduce(Equation& v, Bitmap& coefficients_out) const = 0;

    /// @brief Reduce `v` only until its leading column is not a pivot.
    virtual ReduceResult partial_reduce(Equation& v) const = 0;

    /// @brief Fully reduce `v` against this matrix augmented with `extra`.
    virtual void reduce_with_extra(Equation& v, const Equation& extra) const = 0;

    /// @brief Partially reduce `v` against this matrix augmented with `extra`.
    virtual ReduceResult partial_reduce_with_extra(Equation& v, const Equation& extra) const = 0;

    /// @brief Remove and return the first row.
    virtual Equation pop_first_row() = 0;

    /// @brief Remove the last row.
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
/// - `pivot_columns()` and `row_for_pivot_column()` describe exactly the pivots
///
/// `col(std::size_t)` is deliberately absent. It had three call sites in the old
/// tree and is the one operation that is genuinely awful for a sparse backend,
/// so those sites were rewritten instead.
class RowEchelonMatrix {
public:
    /// @brief Construct an empty matrix over `num_vars` variables.
    /// @param backend Storage representation to use.
    /// @param num_vars Number of variable columns; the constant column is added.
    RowEchelonMatrix(Algebra backend, std::size_t num_vars);

    RowEchelonMatrix(const RowEchelonMatrix& other);
    RowEchelonMatrix(RowEchelonMatrix&& other) noexcept = default;
    RowEchelonMatrix& operator=(const RowEchelonMatrix& other);
    RowEchelonMatrix& operator=(RowEchelonMatrix&& other) noexcept = default;
    virtual ~RowEchelonMatrix() = default;

    /// @brief Which backend stores this matrix.
    Algebra backend() const noexcept { return storage_->backend(); }

    /// @brief Number of stored rows, i.e. the rank.
    std::size_t rows() const noexcept { return storage_->rows(); }

    /// @brief Number of columns, i.e. `num_vars() + 1`.
    std::size_t cols() const noexcept { return storage_->num_vars() + 1; }

    /// @brief Number of variable columns.
    std::size_t num_vars() const noexcept { return storage_->num_vars(); }

    /// @brief Row `index`, in row-echelon order.
    /// @pre `index < rows()`.
    const Equation& row(std::size_t index) const { return storage_->row(index); }

    /// @brief Bitmap of the pivot columns, of width `cols()`.
    const Bitmap& pivot_columns() const noexcept { return storage_->pivot_columns(); }

    /// @brief Index of the row whose leading column is `column`.
    /// @param column A pivot column.
    /// @pre `column < cols()` and `pivot_columns().test(column)`.
    std::size_t row_for_pivot_column(std::size_t column) const {
        return storage_->row_for_pivot_column(column);
    }

    /// @brief Add a row into the matrix.
    /// @param v Row to insert.
    /// @pre `v.size() == cols()` and `v.backend() == backend()`.
    /// @note Does nothing if `v` already belongs to the span.
    void add_row(const Equation& v) { storage_->add_row_get_index(v); }

    /// @brief Add a row and return its inserted index if it was new.
    /// @param v Row to insert.
    /// @return The inserted row index, or `std::nullopt` if `v` already belongs to the span.
    /// @pre `v.size() == cols()` and `v.backend() == backend()`.
    std::optional<std::size_t> add_row_get_index(const Equation& v) {
        return storage_->add_row_get_index(v);
    }

    /// @brief Fully reduce `v` in place against this matrix.
    /// @param v The equation to reduce.
    /// @pre `v.size() == cols()`.
    void reduce(Equation& v) const { storage_->reduce(v); }

    /// @brief Fully reduce `v` in place, recording which rows participated.
    /// @param v The equation to reduce.
    /// @param coefficients_out Receives one bit per row, resized to `rows()`.
    /// @pre `v.size() == cols()`.
    void reduce(Equation& v, Bitmap& coefficients_out) const {
        storage_->reduce(v, coefficients_out);
    }

    /// @brief Reduce `v` in place only until its leading column is not a pivot.
    /// @param v The equation to partially reduce.
    /// @return Whether `v` became @f$\mathbf{0}@f$, became @f$\mathbf{1}@f$, or
    /// has an irreducible leading column.
    /// @pre `v.size() == cols()`.
    ReduceResult partial_reduce(Equation& v) const { return storage_->partial_reduce(v); }

    /// @brief Fully reduce `v` against this matrix augmented with `extra`.
    /// @param v The equation to reduce.
    /// @param extra Extra row spanning one dimension outside this matrix.
    /// @pre `v.size() == extra.size() == cols()`.
    /// @pre `extra` is @f$\mathbf{0}@f$, or its leading column is not a pivot of this matrix.
    void reduce_with_extra(Equation& v, const Equation& extra) const {
        storage_->reduce_with_extra(v, extra);
    }

    /// @brief Partially reduce `v` against this matrix augmented with `extra`.
    /// @param v The equation to partially reduce.
    /// @param extra Extra row spanning one dimension outside this matrix.
    /// @return The same three-way classification as `partial_reduce()`.
    /// @pre `v.size() == extra.size() == cols()`.
    /// @pre `extra` is @f$\mathbf{0}@f$, or its leading column is not a pivot of this matrix.
    ReduceResult partial_reduce_with_extra(Equation& v, const Equation& extra) const {
        return storage_->partial_reduce_with_extra(v, extra);
    }

    /// @brief Remove the first row from the matrix and return it.
    /// @return The removed first row.
    /// @pre `rows() > 0`.
    Equation pop_first_row() { return storage_->pop_first_row(); }

    /// @brief Remove the last row from the matrix.
    /// @pre `rows() > 0`.
    void pop_last_row() { storage_->pop_last_row(); }

    /// @brief Read-only iterator over rows, in row-echelon order.
    class iterator {
    public:
        /// @brief Construct an iterator at row `index` of `matrix`.
        iterator(const RowEchelonMatrix* matrix, std::size_t index) noexcept
            : matrix_(matrix), index_(index) {}

        /// @brief Advance to the next row.
        iterator& operator++() noexcept {
            ++index_;
            return *this;
        }

        /// @brief Dereference to the current row.
        const Equation& operator*() const { return matrix_->row(index_); }

        /// @brief Compare iterators by row index.
        bool operator==(const iterator& other) const noexcept = default;

    private:
        const RowEchelonMatrix* matrix_;
        std::size_t index_;
    };

    /// @brief Begin iterator over rows.
    iterator begin() const noexcept { return iterator(this, 0); }

    /// @brief End iterator over rows.
    iterator end() const noexcept { return iterator(this, rows()); }

protected:
    /// @brief Access the backend storage. For use inside `src/algebra/` only.
    RowEchelonMatrixStorage& storage() const noexcept { return *storage_; }

private:
    std::unique_ptr<RowEchelonMatrixStorage> storage_;
};

/// @brief A subspace of the space of linear equations over GF(2), held as a
/// basis in row-echelon form.
///
/// Each column corresponds to a variable in the original order.
class RowSpace : public RowEchelonMatrix {
public:
    using RowEchelonMatrix::RowEchelonMatrix;

    /// @brief Get the unique solution to the system of equations this space represents.
    /// @return One bit per variable, of width `num_vars()`.
    /// @pre The row space has exactly one solution; equivalently, every variable
    /// column is pivoted and the constant column is not.
    Bitmap unique_solution() const;
};

/// @brief A clause in its linear negation, as the conjunction of the linear
/// equations whose common solutions are exactly where the clause is false.
///
/// The paper writes this as @f$\mathrm{neg}(C) = \mathrm{span}(f_i +
/// \mathbf{1})@f$. For example the clause `[x1 + x2 = 1] v [x3 = 0]` is stored
/// as `[x1 + x2 = 0] ^ [x3 = 1]`, i.e. `[[1, 1, 0, 0], [0, 0, 1, 1]]`.
class Clause : public RowSpace {
public:
    using RowSpace::RowSpace;

    /// @brief Index of the first watched equation.
    std::size_t watched0 = 0;

    /// @brief Index of the second watched equation.
    std::size_t watched1 = 1;

    /// @brief Starting index for the next cyclic search for a replacement watch.
    std::size_t next_watch_search_start = 0;

    /// @brief Render the clause as a human-readable string.
    /// @param mode Rendering mode for equation and clause formatting.
    std::string to_string(const PrintMode& mode = {}) const;
};

} // namespace xorcle
