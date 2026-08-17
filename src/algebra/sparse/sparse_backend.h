#pragma once

#include <cstddef>
#include <memory>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"

namespace xorcle {

// Sorted-index storage over `num_vars + 1` columns.

std::unique_ptr<EquationStorage> make_sparse_equation_storage(std::size_t num_vars);

std::unique_ptr<RowEchelonMatrixStorage> make_sparse_matrix_storage(std::size_t num_vars);

} // namespace xorcle
