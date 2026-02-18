#pragma once

#include <cstddef>
#include <vector>

#include "algebra/Equation.h"
#include "algebra/RowSpace.h"

namespace util {

/// @brief Choose a decision from a basis of equations that is not yet implied by the trail.
/// @pre basis contains num_vars unit equations or an equivalent basis.
algebra::Equation decide_from_basis(const std::vector<algebra::Equation>& basis,
                                    const algebra::RowSpace& trail_span,
                                    std::size_t num_vars);

/// @brief Choose a decision from unit equations that is not yet implied by the trail.
/// @pre unit_eqns contains exactly num_vars unit equations.
algebra::Equation decide_from_units(const algebra::RowSpace& trail_span,
                                    const std::vector<algebra::Equation>& unit_eqns,
                                    std::size_t num_vars);

/// @brief Return true if the clause is already satisfied by the trail.
bool clause_satisfied_by_trail(const algebra::RowSpace& trail_span,
                               const algebra::Clause& clause,
                               std::size_t num_vars);

/// @brief Sample a non-zero equation from a clause with respect to the trail.
/// @param clause Clause to sample from.
/// @param trail_span Current trail span.
/// @param num_vars Number of variables in the problem.
/// @param out_candidate Output equation sampled from the clause.
/// @param out_reduced Output equation reduced by the trail span.
/// @return True if a non-zero reduced equation was produced.
bool sample_equation_from_clause(const algebra::Clause& clause,
                                 const algebra::RowSpace& trail_span,
                                 std::size_t num_vars,
                                 algebra::Equation& out_candidate,
                                 algebra::Equation& out_reduced);

} // namespace util
