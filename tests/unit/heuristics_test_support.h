#pragma once

#include <cstddef>
#include <initializer_list>

#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"

namespace xorcle {

/// @brief Build a clause from its rows, each given as a list of set bits in the
/// linear negation, where bit `num_vars` is the constant bit.
///
/// The watch fields keep `Clause`'s own defaults: `watched0 = 0` and
/// `watched1 = 1`.
Clause make_clause(EquationRepr repr, std::size_t num_vars,
                   std::initializer_list<std::initializer_list<std::size_t>> rows);

/// @brief Build a row space from its rows, given as lists of set bits.
RowSpace make_row_space(EquationRepr repr, std::size_t num_vars,
                        std::initializer_list<std::initializer_list<std::size_t>> rows);

} // namespace xorcle
