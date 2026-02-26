#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "bit/matrix.h"
#include "bit/vector.h"

namespace algebra {

/// @brief Classification of an affine equation after partial reduction.
///
/// The last column is interpreted as the affine constant term.
enum class ReduceResult {
    Zero,       ///< The equation partially reduces to 0 = 0.
    One,        ///< The equation partially reduces to 0 = 1.
    Nontrivial  ///< The equation partially reduces to an irreducible variable-leading equation.
};

/// @brief A matrix over GF(2) in row-echelon form.
///
/// Used to represent equation spaces and to perform reductions.
class REFMatrix {
private:
    /// @brief Vector of size M.cols() that is set to 1 for each pivot column.
    bit::vector<> pivot_cols;

    /// @brief Convert from pivot column to row index (undefined if column is not pivot).
    std::vector<std::size_t> col_to_row;

protected:
    /// @brief Underlying matrix data in row-echelon form.
    bit::matrix<> M;

public:
    /// @brief Construct an empty REFMatrix for a given variable count.
    explicit REFMatrix(std::size_t num_vars) {
        pivot_cols = bit::vector<>::zeros(num_vars + 1);
        col_to_row = std::vector<std::size_t>(num_vars + 1);
    };

    /// @brief Construct a REFMatrix from a matrix.
    /// @pre M is non-empty.
    explicit REFMatrix(bit::matrix<> M);

    /// @brief Return the pivot column bitmap.
    const bit::vector<>& get_pivot_cols() const { return pivot_cols; }

    /// @brief Return the row index for a pivot column.
    /// @pre i is a pivot column.
    const std::size_t get_col_to_row(std::size_t i) const { return col_to_row[i]; }

    /// @brief Add a row into the matrix (copy semantics).
    /// @note Does nothing if v already belongs to the span.
    void add_row(const bit::vector<>& v);

    /// @brief Add a row and return its inserted index if it was new.
    std::optional<std::size_t> add_row_get_index(const bit::vector<>& v);

    /// @brief Reduce a vector in-place using this matrix.
    ///
    /// Interpret each row as a rule that replaces the leading term with the tail.
    /// After this, the vector is 0 in all pivot columns.
    /// @param v The vector to reduce.
    /// @param coeffs If present, v = result + coeffs * this, i.e. bits
    /// corresponding to rows used in the reduction will be set.
    void reduce(bit::vector<>& v, bit::vector<>* coeffs = nullptr) const;

    /// @brief Partially reduce a vector in lex order and classify the result.
    ///
    /// Repeatedly eliminates the current leading term while it is pivoted. Stops as soon as the
    /// leading term is not pivoted. This is sufficient to detect 0 = 0 and 0 = 1 exactly.
    /// @param v The vector to partially reduce in place.
    /// @return Classification of the partially reduced affine equation.
    ReduceResult partial_reduce(bit::vector<>& v) const;

    /// @brief Partially reduce a vector using this matrix augmented with an extra vector.
    /// @pre extra is already partially reduced by this matrix (it may be 0 = 1).
    /// @return Classification of the partially reduced affine equation.
    ReduceResult partial_reduce_with_extra(bit::vector<>& v, const bit::vector<>& extra) const;

    /// @brief Reduce a vector using this matrix augmented with an extra vector.
    /// @pre extra is already reduced by this matrix.
    void reduce_with_extra(bit::vector<>& v, const bit::vector<>& extra) const;

    /// @brief Remove the first row from the matrix and return it.
    bit::vector<> pop_first_row();

    /// @brief Number of rows in the matrix.
    std::size_t rows() const { return M.rows(); }

    /// @brief Number of columns in the matrix.
    std::size_t cols() const { return M.cols(); }

    /// @brief Return a row by index.
    const bit::vector<>& row(std::size_t i) const { return M.row(i); }

    /// @brief Return a column by index.
    const bit::vector<> col(std::size_t i) const { return M.col(i); }

    /// @brief Remove the last row from the matrix.
    void pop_last_row() { M.pop_row(); }

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
