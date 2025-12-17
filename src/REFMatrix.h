#pragma once

#include "bit/vector.h"
#include "bit/matrix.h"

/// @brief A matrix over GF(2) in row-echelon form. Used to represent
/// both equation space and equation space in different bases. 
class REFMatrix {
private:
    // Vector of size M.cols() that is set to 1 for each pivot column
    bit::vector<> pivot_cols;

    // Convert from pivot column to row index, undefined behavior if not pivot
    std::vector<std::size_t> col_to_row;

protected:
    bit::matrix<> M;

public:
    REFMatrix() {};

    /// @brief Construct a REFMatrix from a matrix.
    /// Requires: M is non-empty.
    REFMatrix(bit::matrix<> M);

    /// @brief Add a row into the matrix. Uses copy semantics. Does nothing if v
    /// already belongs to the span of the matrix.
    /// 
    /// @param v The new row to push
    /// @param inserted_index If present, on return this will have the index in this matrix
    /// to which a new row was inserted
    void add_row(const bit::vector<>& v);
    
    std::optional<std::size_t> add_row_get_index(const bit::vector<>& v);

    /// @brief Use this matrix to reduce a vector in-place.
    ///
    /// Interpret each row of the matrix as a rule that replaces the leading term with the tail.
    /// After this, the vector is 0 in all pivot columns.
    /// @param v The vector to reduce.
    /// @param coeffs If present, v = result + coeffs * this, i.e. the bits corresponding to
    /// rows used in the reduction will be set to 1.
    void reduce(bit::vector<>& v, bit::vector<>* coeffs = nullptr) const;

    /// @brief Use this matrix to reduce a vector and return a copy.
    ///
    /// Interpret each row of the matrix as a rule that replaces the leading term with the tail.
    /// After this, the vector is 0 in all pivot columns.
    /// @param v The vector to reduce.
    /// @param coeffs If present, v = result + coeffs * this, i.e. the bits corresponding to
    /// rows used in the reduction will be set to 1.
    bit::vector<> reduce_copy(const bit::vector<>& v,  bit::vector<>* coeffs = nullptr) const;

    /// @brief Use this matrix augmented with an extra vector to reduce in-place.
    /// 
    /// Requires: The extra vector must already be reduced by this matrix.
    /// @param v The vector to reduce.
    /// @param extra The extra vector to reduce by.
    void reduce_with_extra(bit::vector<>& v, const bit::vector<>& extra) const;

    /// @brief Use this matrix augmented with an extra vector to reduce and return a copy.
    /// 
    /// Requires: The extra vector must already be reduced by this matrix.
    /// @param v The vector to reduce.
    /// @param extra The extra vector the reduce by.
    bit::vector<> reduce_with_extra_copy(const bit::vector<>& v, const bit::vector<>& extra) const;

    /// @brief Remove the first row from the matrix and return it.
    bit::vector<> pop_first_row();

    // Pass through various matrix functions
    std::size_t rows() const { return M.rows(); }
    std::size_t cols() const { return M.cols(); }
    const bit::vector<>& row(std::size_t i) const { return M.row(i); }
    const bit::vector<> col(std::size_t i) const { return M.col(i); }
    void pop_last_row() { M.pop_row(); }

    // Basic support for read-only for-each loops over rows
    struct iterator {
        const REFMatrix* M;
        std::size_t i;
        iterator(const REFMatrix* M, std::size_t i) : M(M), i(i) {}
        iterator& operator++() { ++i; return *this; }
        const bit::vector<>& operator*() const { return M->row(i); }
        bool operator!=(const iterator& other) const { return i != other.i; }
    };
    iterator begin() const { return iterator(this, 0); }
    iterator end() const { return iterator(this, M.rows()); }
};