#include "algebra/equation_repr.h"

#include <cassert>

#include "algebra/dense/dense_repr.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra/sparse/sparse_repr.h"

namespace xorcle {

std::string_view to_string(EquationRepr repr) noexcept {
    switch (repr) {
        case EquationRepr::dense:
            return "dense";
        case EquationRepr::sparse:
            return "sparse";
    }
    assert(false && "to_string: not a representation");
    return "dense";
}

std::optional<EquationRepr> equation_repr_from_string(std::string_view name) noexcept {
    if (name == "dense") {
        return EquationRepr::dense;
    }
    if (name == "sparse") {
        return EquationRepr::sparse;
    }
    return std::nullopt;
}

std::unique_ptr<EquationInterface> make_equation_interface(EquationRepr repr,
                                                           std::size_t num_bits) {
    switch (repr) {
        case EquationRepr::dense:
            return make_dense_equation_interface(num_bits);
        case EquationRepr::sparse:
            return make_sparse_equation_interface(num_bits);
    }
    assert(false && "make_equation_interface: not a representation");
    return make_dense_equation_interface(num_bits);
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
    return make_dense_row_echelon_matrix_interface(cols);
}

Equation make_equation(EquationRepr repr, std::size_t num_vars) {
    return Equation(repr, num_vars);
}

RowEchelonMatrix make_row_echelon_matrix(EquationRepr repr, std::size_t cols) {
    return RowEchelonMatrix(repr, cols);
}

} // namespace xorcle
