#include "algebra/sparse/sparse_backend.h"

#include "algebra/equation_impl.h"
#include "algebra/row_echelon_matrix_impl.h"
#include "algebra/sparse/sparse_row.h"

namespace xorcle {

// The shared templated algorithms, instantiated for the sparse backend. Nothing
// here differs from dense_backend.cpp but the row type: the algorithms are
// written once.
static_assert(Row<SparseRow>, "SparseRow must satisfy the Row concept");

std::unique_ptr<EquationStorage> make_sparse_equation_storage(std::size_t num_vars) {
    return std::make_unique<EquationImpl<SparseRow>>(num_vars + 1);
}

std::unique_ptr<RowEchelonMatrixStorage> make_sparse_matrix_storage(std::size_t num_vars) {
    return std::make_unique<RowEchelonMatrixImpl<SparseRow>>(num_vars);
}

} // namespace xorcle
