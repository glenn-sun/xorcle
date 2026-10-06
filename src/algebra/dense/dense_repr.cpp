#include "algebra/dense/dense_repr.h"

#include "algebra/dense/dense_gf2_vector.h"
#include "algebra/equation_template.h"
#include "algebra/row_echelon_matrix_template.h"

namespace xorcle {

static_assert(GF2Vector<DenseGF2Vector>,
              "DenseGF2Vector must satisfy the GF2Vector concept");

std::unique_ptr<EquationInterface> make_dense_equation_interface(std::size_t num_bits) {
    return EquationTemplate<DenseGF2Vector>::create(num_bits);
}

std::unique_ptr<RowEchelonMatrixInterface> make_dense_row_echelon_matrix_interface(
    std::size_t cols) {
    return std::make_unique<RowEchelonMatrixTemplate<DenseGF2Vector>>(cols);
}

} // namespace xorcle
