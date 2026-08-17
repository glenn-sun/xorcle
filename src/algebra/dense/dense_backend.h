#pragma once

#include <cstddef>
#include <memory>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"

namespace xorcle {

// Bit-packed storage over `num_vars + 1` columns. These signatures mention only
// our own types, so callers never see gf2.

std::unique_ptr<EquationStorage> make_dense_equation_storage(std::size_t num_vars);

std::unique_ptr<RowEchelonMatrixStorage> make_dense_matrix_storage(std::size_t num_vars);

} // namespace xorcle
