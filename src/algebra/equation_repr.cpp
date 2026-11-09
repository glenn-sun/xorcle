#include "algebra/equation_repr.h"

#include <memory>
#include <type_traits>

#include "algebra/equation.h"
#include "algebra/equation_template.h"
#include "algebra/gf2_vector_of_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra/row_echelon_matrix_template.h"

namespace xorcle {

std::unique_ptr<EquationInterface> make_equation_interface(EquationRepr repr,
                                                           std::size_t num_bits) {
    return with_gf2_vector_of(
        repr, [num_bits]<GF2Vector V>(std::type_identity<V>) -> std::unique_ptr<EquationInterface> {
            return EquationTemplate<V>::create(num_bits);
        });
}

std::unique_ptr<RowEchelonMatrixInterface> make_row_echelon_matrix_interface(
    EquationRepr repr, std::size_t cols) {
    return with_gf2_vector_of(
        repr, [cols]<GF2Vector V>(std::type_identity<V>) -> std::unique_ptr<RowEchelonMatrixInterface> {
            return std::make_unique<RowEchelonMatrixTemplate<V>>(cols);
        });
}

Equation make_equation(EquationRepr repr, std::size_t num_vars) {
    return Equation(repr, num_vars);
}

} // namespace xorcle
