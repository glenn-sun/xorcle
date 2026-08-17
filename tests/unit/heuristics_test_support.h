#pragma once

#include <cstddef>
#include <initializer_list>

#include "algebra/algebra.h"
#include "algebra/row_echelon_matrix.h"

namespace xorcle {

/// @brief Build a clause from its rows, each given as a list of set columns in
/// the linear negation, where column `num_vars` is the constant column.
///
/// The watch fields keep `Clause`'s own defaults, which is what a parsed clause
/// gets: `watched0 = 0` and `watched1 = 1`, the latter deliberately out of range
/// for a unit clause.
Clause make_clause(Algebra backend, std::size_t num_vars,
                   std::initializer_list<std::initializer_list<std::size_t>> rows);

/// @brief Build a row space from its rows, given as lists of set columns.
RowSpace make_row_space(Algebra backend, std::size_t num_vars,
                        std::initializer_list<std::initializer_list<std::size_t>> rows);

} // namespace xorcle
