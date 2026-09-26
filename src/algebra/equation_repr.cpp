#include "algebra/equation_repr.h"

#include <cassert>
#include <utility>

#include "algebra/dense/dense_repr.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra/sparse/sparse_repr.h"

namespace xorcle {

std::unique_ptr<EquationInterface> make_equation_interface(EquationRepr repr,
                                                           std::size_t num_bits) {
    switch (repr) {
        case EquationRepr::dense:
            return make_dense_equation_interface(num_bits);
        case EquationRepr::sparse:
            return make_sparse_equation_interface(num_bits);
    }
    assert(false && "make_equation_interface: not a representation");
    std::unreachable();
}

std::unique_ptr<RowEchelonMatrixInterface> make_row_echelon_matrix_interface(
    EquationRepr repr, std::size_t cols) {
    switch (repr) {
        case EquationRepr::dense:
            return make_dense_row_echelon_matrix_interface(cols);
        case EquationRepr::sparse:
            return make_sparse_row_echelon_matrix_interface(cols);
    }
    assert(false && "make_row_echelon_matrix_interface: not a representation");
    std::unreachable();
}

Equation make_equation(EquationRepr repr, std::size_t num_vars) {
    return Equation(repr, num_vars);
}

} // namespace xorcle
