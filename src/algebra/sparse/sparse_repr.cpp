#include "algebra/sparse/sparse_repr.h"

#include "algebra/equation_template.h"
#include "algebra/row_echelon_matrix_template.h"
#include "algebra/sparse/sparse_gf2_vector.h"

namespace xorcle {

static_assert(GF2Vector<SparseGF2Vector>,
              "SparseGF2Vector must satisfy the GF2Vector concept");

std::unique_ptr<EquationInterface> make_sparse_equation_interface(std::size_t num_vars) {
    return std::make_unique<EquationTemplate<SparseGF2Vector>>(num_vars + 1);
}

std::unique_ptr<RowEchelonMatrixInterface> make_sparse_row_echelon_matrix_interface(
    std::size_t num_vars) {
    return std::make_unique<RowEchelonMatrixTemplate<SparseGF2Vector>>(num_vars);
}

} // namespace xorcle
