#pragma once

#include <cstddef>
#include <memory>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"

namespace xorcle {

// Sorted-index storage over `num_vars + 1` bits.

std::unique_ptr<EquationInterface> make_sparse_equation_interface(std::size_t num_vars);

std::unique_ptr<RowEchelonMatrixInterface> make_sparse_row_echelon_matrix_interface(
    std::size_t num_vars);

} // namespace xorcle
