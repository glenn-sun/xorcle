#pragma once

#include <cstddef>
#include <memory>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"

namespace xorcle {

std::unique_ptr<EquationInterface> make_dense_equation_interface(std::size_t num_vars);

std::unique_ptr<RowEchelonMatrixInterface> make_dense_row_echelon_matrix_interface(
    std::size_t num_vars);

} // namespace xorcle
