#pragma once

#include <cstddef>
#include <memory>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"

namespace xorcle {

/// @brief Build sparse equation storage.
/// @param num_vars Number of variable columns; the constant column is added.
/// @return An all-zero sorted-index row of width `num_vars + 1`.
std::unique_ptr<EquationStorage> make_sparse_equation_storage(std::size_t num_vars);

/// @brief Build sparse row-echelon matrix storage.
/// @param num_vars Number of variable columns; the constant column is added.
/// @return An empty sorted-index matrix over `num_vars + 1` columns.
std::unique_ptr<RowEchelonMatrixStorage> make_sparse_matrix_storage(std::size_t num_vars);

} // namespace xorcle
