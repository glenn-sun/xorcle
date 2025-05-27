#include "bit/vector.h"
#include "bit/matrix.h"

class REFMatrix {
private:
    bit::matrix<> M;

public:
    REFMatrix(bit::matrix<> M) {
        this->M = M.to_echelon_form();
    }
};


    // /// @brief Use this matrix to reduce a vector in-place.
    // /// 
    // /// Requires: This matrix must be in row-echelon form.
    // ///
    // /// Interpret each row of the matrix as a rule that replaces the leading term with the tail.
    // /// After this, the vector is 0 in all pivot columns.
    // /// @param r The vector to reduce.
    // constexpr vector_type& reduce(vector_type& r) const
    // {
    //     bit_verify(pivot_cols.has_value(), "Matrix is not in row-echelon form!");

    //     std::size_t pivot_row = 0;
    //     pivot_cols->if_set_call([&](std::size_t i) { 
    //         if (r.element(i)) {
    //             r ^= row(pivot_row);
    //         }
    //         ++pivot_row;
    //     });
    //     return r;
    // }

    // /// @brief Use this matrix to reduce a vector and return a copy.
    // /// 
    // /// Requires: This matrix must be in row-echelon form.
    // ///
    // /// Interpret each row of the matrix as a rule that replaces the leading term with the tail.
    // /// After this, the vector is 0 in all pivot columns.
    // /// @param r The vector to reduce.
    // constexpr vector_type reduce_copy(const vector_type& r) const 
    // {
    //     vector_type r_copy = r;
    //     return reduce(r_copy);
    // }

    // /// @brief Use this matrix augmented with an extra vector to reduce in-place.
    // /// 
    // /// Requires: This matrix must be in row-echelon form. The extra vector must already be
    // /// reduced by this matrix.
    // /// @param r The vector to reduce.
    // /// @param extra The extra vector the reduce by.
    // constexpr vector_type& reduce_with(vector_type& r, const vector_type& extra) const
    // {
    //     bit_verify(pivot_cols.has_value(), "Matrix is not in row-echelon form!");

    //     std::size_t extra_col = extra.first_set();

    //     std::size_t pivot_row = 0;
    //     for (std::size_t i = 0; i < rows(); ++i) {
    //         if (i == extra_col && r.element(i)) {
    //             r ^= extra;
    //         } else if (pivot_cols->element(i)) {
    //             if (r.element(i)) {
    //                 r ^= row(pivot_row);
    //             }
    //             ++pivot_row;
    //         }
    //     }

    //     return r;
    // }

    // /// @brief Use this matrix augmented with an extra vector to reduce and return a copy.
    // /// 
    // /// Requires: This matrix must be in row-echelon form. The extra vector must already be
    // /// reduced by this matrix.
    // /// @param r The vector to reduce.
    // /// @param extra The extra vector the reduce by.
    // constexpr vector_type reduce_with_copy(const vector_type& r, const vector_type& extra) const 
    // {
    //     vector_type r_copy = r;
    //     return reduce_with(r_copy, extra);
    // }

    // /// @brief Determine if the span of the rows of this matrix contains the given vector.
    // ///
    // /// Requires: This matrix must be in row-echelon form.
    // /// @param v The vector to check
    // constexpr bool span_contains(const vector_type& v) const 
    // {
    //     return reduce_copy(v).none(); 
    // }

    // /// @brief Push a row into the matrix and maintain row-echelon form.
    // /// Uses copy semantics. Effectively O(r.size()).
    // /// 
    // /// Requires: The matrix is already in row-echelon form.
    // /// @param r The new row to push
    // matrix& push_to_echelon_form(const vector_type& r, std::size_t* inserted_index = nullptr)
    // {
    //     bit_verify(pivot_cols.has_value() || rows() == 0, "Matrix is not in row-echelon form!");

    //     if (rows() == 0) {
    //         pivot_cols = std::make_optional<vector_type>(vector_type::zeros(r.size()));
    //         pivot_cols->set(r.first_set());
    //         push_row(r);
    //         return *this;
    //     }

    //     vector_type new_row = r;

    //     // Reduce the vector up to the first irreducible column
    //     std::size_t pivot_row = 0;
    //     std::size_t pivot_col = 0;
    //     for (; pivot_col < pivot_cols->size(); ++pivot_col) {
    //         if (new_row.element(pivot_col)) {
    //             if (pivot_cols->element(pivot_col)) {
    //                 new_row ^= row(pivot_row);
    //             }
    //             else {
    //                 break;
    //             }
    //         }
    //         pivot_row += pivot_cols->element(pivot_col);
    //     }

    //     // If the vector is now 0, don't do anything
    //     if (new_row.none()) {
    //         return *this;
    //     }

    //     // Insert the new row
    //     m_row.insert(m_row.begin() + pivot_row, new_row);
    //     pivot_cols->set(pivot_col);
    //     if (inserted_index) *inserted_index = pivot_row;

    //     return *this;
    // }

    // // /// @brief Push a row into the matrix and maintain reduced-row-echelon form.
    // // /// Uses copy semantics. Effectively O(r.size()).
    // // /// 
    // // /// Requires: The matrix is already in reduced-row-echelon form.
    // // /// @param r The new row to push
    // // matrix& push_to_reduced_echelon_form(const vector_type& r) {
    // //     bit_verify(pivot_cols.has_value() || rows() == 0, "Matrix is not in row-echelon form!");

    // //     if (rows() == 0) {
    // //         pivot_cols = std::make_optional<vector_type>(vector_type::zeros(r.size()));
    // //         pivot_cols->set(r.first_set());
    // //         push_row(r);
    // //         return *this;
    // //     }

    // //     vector_type new_row = reduce_copy(r);

    // //     // If the vector is now 0, don't do anything
    // //     if (new_row.none()) {
    // //         return *this;
    // //     }
        
    // //     // Find the row to swap into
    // //     std::size_t pivot_col = new_row.first_set();
    // //     std::size_t pivot_row = 0;
    // //     pivot_cols->if_set_call([&](std::size_t i) { 
    // //         if (pivot_col > i) {
    // //             ++pivot_row;
    // //         }
    // //     });

    // //     // Insert the new row
    // //     m_row.insert(m_row.begin() + pivot_row, new_row);
    // //     pivot_cols->set(pivot_col);

    // //     // Clear above the new pivot
    // //     for (std::size_t i = 0; i < pivot_row; ++i) {
    // //         if (element(i, pivot_col)) {
    // //             row(i) ^= row(pivot_row);
    // //         }
    // //     }

    // //     return *this;
    // // }