#include "algebra/equation_repr.h"

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

// Only the per-representation factory headers are included here, and their
// signatures mention nothing but our own types -- that is what keeps gf2
// confined to `algebra/dense/`.

std::unique_ptr<EquationInterface> make_equation_interface(EquationRepr repr,
                                                           std::size_t num_vars) {
    switch (repr) {
        case EquationRepr::dense:
            return make_dense_equation_interface(num_vars);
        case EquationRepr::sparse:
            return make_sparse_equation_interface(num_vars);
    }
    return make_dense_equation_interface(num_vars);
}

std::unique_ptr<RowEchelonMatrixInterface> make_row_echelon_matrix_interface(
    EquationRepr repr, std::size_t num_vars) {
    switch (repr) {
        case EquationRepr::dense:
            return make_dense_row_echelon_matrix_interface(num_vars);
        case EquationRepr::sparse:
            return make_sparse_row_echelon_matrix_interface(num_vars);
    }
    return make_dense_row_echelon_matrix_interface(num_vars);
}

Equation make_equation(EquationRepr repr, std::size_t num_vars) {
    return Equation(repr, num_vars);
}

RowEchelonMatrix make_row_echelon_matrix(EquationRepr repr, std::size_t num_vars) {
    return RowEchelonMatrix(repr, num_vars);
}

} // namespace xorcle
