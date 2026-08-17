#pragma once

#include <cstddef>
#include <memory>

#include "algebra/algebra.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"

namespace xorcle {

// Each of these adds the constant column, so widths are `num_vars + 1`.

std::unique_ptr<EquationStorage> make_equation_storage(Algebra backend, std::size_t num_vars);

std::unique_ptr<RowEchelonMatrixStorage> make_matrix_storage(Algebra backend,
                                                             std::size_t num_vars);

/// @brief Build the true equation @f$\mathbf{0}@f$ on the chosen backend.
Equation make_equation(Algebra backend, std::size_t num_vars);

RowEchelonMatrix make_matrix(Algebra backend, std::size_t num_vars);

} // namespace xorcle
