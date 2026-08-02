#pragma once

#include <cstddef>
#include <memory>

#include "algebra/algebra.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"

namespace xorcle {

/// @brief Build the equation storage for a backend.
/// @param backend Storage representation to use.
/// @param num_vars Number of variable columns; the constant column is added.
/// @return An all-zero row of width `num_vars + 1`.
std::unique_ptr<EquationStorage> make_equation_storage(Algebra backend, std::size_t num_vars);

/// @brief Build the row-echelon matrix storage for a backend.
/// @param backend Storage representation to use.
/// @param num_vars Number of variable columns; the constant column is added.
/// @return An empty matrix over `num_vars + 1` columns.
std::unique_ptr<RowEchelonMatrixStorage> make_matrix_storage(Algebra backend,
                                                             std::size_t num_vars);

/// @brief Build the true equation @f$\mathbf{0}@f$ on the chosen backend.
/// @param backend Storage representation to use.
/// @param num_vars Number of variable columns.
Equation make_equation(Algebra backend, std::size_t num_vars);

/// @brief Build an empty row-echelon matrix on the chosen backend.
/// @param backend Storage representation to use.
/// @param num_vars Number of variable columns.
RowEchelonMatrix make_matrix(Algebra backend, std::size_t num_vars);

} // namespace xorcle
