#pragma once

#include <cstddef>
#include <memory>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"

namespace xorcle {

/// @brief Build dense equation storage.
/// @param num_vars Number of variable columns; the constant column is added.
/// @return An all-zero bit-packed row of width `num_vars + 1`.
/// @note The signature mentions only our own types, so callers never see gf2.
std::unique_ptr<EquationStorage> make_dense_equation_storage(std::size_t num_vars);

/// @brief Build dense row-echelon matrix storage.
/// @param num_vars Number of variable columns; the constant column is added.
/// @return An empty bit-packed matrix over `num_vars + 1` columns.
std::unique_ptr<RowEchelonMatrixStorage> make_dense_matrix_storage(std::size_t num_vars);

} // namespace xorcle
