#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <vector>

#include "bit/matrix.h"
#include "bit/vector.h"

namespace algebra {

/// @brief A matrix over GF(2) in row-echelon form.
///
/// @note Representation invariants:
/// - `pivot_cols.size() == num_vars + 1`
/// - `col_to_row.size() == num_vars + 1`
/// - if `rows() > 0`, every stored row has width `num_vars + 1`
/// - stored rows are in row-echelon form with strictly increasing pivot columns
/// - `pivot_cols` and `col_to_row` describe exactly the pivots of `M`
class REFMatrix {
private:
    /// @brief Bitmap of pivot columns in the represented equation width.
    bit::vector<> pivot_cols;

    /// @brief Map from pivot column to row index.
    std::vector<std::size_t> col_to_row;

    std::size_t num_vars;

    void validate_operand_width(const bit::vector<>& v, const char* where) const;

#ifndef NDEBUG
    void debug_assert_valid_representation() const;
#endif

protected:
    /// @brief Underlying matrix data in row-echelon form.
    bit::matrix<> M;

public:
    /// @brief Construct an empty REFMatrix for a given variable count.
    /// @param num_vars Number of variable columns. The constant column is added internally.
    explicit REFMatrix(std::size_t num_vars)
        : pivot_cols(bit::vector<>::zeros(num_vars + 1)),
          col_to_row(num_vars + 1, 0),
          num_vars(num_vars),
          M() {}

    /// @brief Construct a REFMatrix from a matrix.
    /// @param M Input matrix whose columns represent variables plus the constant column.
    /// @pre `M` is non-empty.
    /// @throws std::invalid_argument if `M` is empty.
    explicit REFMatrix(bit::matrix<> M);

    /// @brief Return the pivot column bitmap.
    const bit::vector<>& get_pivot_cols() const { return pivot_cols; }

    /// @brief Return the row index for a pivot column.
    /// @param i Pivot-column index.
    /// @return The row index whose leading term is column `i`.
    /// @pre `i < cols()` and column `i` is a pivot column.
    std::size_t get_col_to_row(std::size_t i) const;

    /// @brief Add a row into the matrix (copy semantics).
    /// @param v Row to insert.
    /// @pre `v.size() == cols()`.
    /// @note Does nothing if `v` already belongs to the span.
    void add_row(const bit::vector<>& v);

    /// @brief Add a row and return its inserted index if it was new.
    /// @param v Row to insert.
    /// @return The inserted row index, or `std::nullopt` if `v` already belongs to the span.
    /// @pre `v.size() == cols()`.
    std::optional<std::size_t> add_row_get_index(const bit::vector<>& v);

    /// @brief Reduce a vector in-place using this matrix.
    /// @param v The vector to reduce.
    /// @param coeffs If present, records which rows participated in the reduction.
    /// @pre `v.size() == cols()`.
    void reduce(bit::vector<>& v, bit::vector<>* coeffs = nullptr) const;

    /// @brief Partially reduce a vector in lex order.
    /// @param v The vector to partially reduce in place.
    /// @return The leading set-bit position after partial reduction:
    /// `bit::vector<>::npos` for `0 = 0`, `num_vars` for `0 = 1`, or a smaller
    /// column index for a variable-leading irreducible row.
    /// @pre `v.size() == cols()`.
    std::size_t partial_reduce(bit::vector<>& v) const;

    /// @brief Partially reduce a vector using this matrix augmented with an extra vector.
    /// @param v The vector to partially reduce.
    /// @param extra Extra row that is already partially reduced by this matrix.
    /// @return The leading set-bit position after partial reduction, using the same
    /// conventions as `partial_reduce()`.
    /// @pre `v.size() == cols()`.
    /// @pre `extra.size() == cols()`.
    /// @pre `extra` is already partially reduced by this matrix.
    std::size_t partial_reduce_with_extra(bit::vector<>& v, const bit::vector<>& extra) const;

    /// @brief Reduce a vector using this matrix augmented with an extra vector.
    /// @param v The vector to reduce.
    /// @param extra Extra row that is already reduced by this matrix.
    /// @pre `v.size() == cols()`.
    /// @pre `extra.size() == cols()`.
    /// @pre `extra` is already reduced by this matrix.
    void reduce_with_extra(bit::vector<>& v, const bit::vector<>& extra) const;

    /// @brief Remove the first row from the matrix and return it.
    /// @return The removed first row.
    /// @pre `rows() > 0`.
    /// @throws std::runtime_error if the matrix is empty.
    /// @throws std::logic_error if the stored first row violates the row-echelon representation.
    bit::vector<> pop_first_row();

    /// @brief Number of rows in the matrix.
    std::size_t rows() const { return M.rows(); }

    /// @brief Number of columns in the represented equation width.
    std::size_t cols() const { return num_vars + 1; }

    /// @brief Return a row by index.
    const bit::vector<>& row(std::size_t i) const { return M.row(i); }

    /// @brief Return a column by index.
    const bit::vector<> col(std::size_t i) const { return M.col(i); }

    /// @brief Remove the last row from the matrix.
    /// @pre `rows() > 0`.
    /// @throws std::runtime_error if the matrix is empty.
    /// @throws std::logic_error if the stored last row violates the row-echelon representation.
    void pop_last_row();

    /// @brief Read-only iterator over rows.
    struct iterator {
        const REFMatrix* M; ///< Matrix being iterated.
        std::size_t i;      ///< Current row index.

        /// @brief Construct an iterator at row i.
        iterator(const REFMatrix* M, std::size_t i) : M(M), i(i) {}

        /// @brief Advance to the next row.
        iterator& operator++() { ++i; return *this; }

        /// @brief Dereference to current row.
        const bit::vector<>& operator*() const { return M->row(i); }

        /// @brief Compare iterators by index.
        bool operator!=(const iterator& other) const { return i != other.i; }
    };

    /// @brief Begin iterator over rows.
    iterator begin() const { return iterator(this, 0); }

    /// @brief End iterator over rows.
    iterator end() const { return iterator(this, M.rows()); }
};
} // namespace algebra
