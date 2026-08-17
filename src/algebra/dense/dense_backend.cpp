#include "algebra/dense/dense_backend.h"

#include "algebra/dense/dense_row.h"
#include "algebra/equation_impl.h"
#include "algebra/row_echelon_matrix_impl.h"

namespace xorcle {

// Instantiates the shared templated algorithms for the dense backend. Everything
// gf2 touches is confined here and in dense_row.{h,cpp}.
static_assert(Row<DenseRow>, "DenseRow must satisfy the Row concept");

std::unique_ptr<EquationStorage> make_dense_equation_storage(std::size_t num_vars) {
    return std::make_unique<EquationImpl<DenseRow>>(num_vars + 1);
}

std::unique_ptr<RowEchelonMatrixStorage> make_dense_matrix_storage(std::size_t num_vars) {
    return std::make_unique<RowEchelonMatrixImpl<DenseRow>>(num_vars);
}

} // namespace xorcle
