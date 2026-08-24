#pragma once

#include <cstddef>
#include <vector>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "core/trail_basis.h"
#include "heuristics/decisions/decision_procedure.h"

namespace xorcle {

/// @brief What conflict analysis derives: a clause and the level it asserts at.
struct AssertingClause {
    Clause clause; ///< In its linear negation.

    /// @brief The level to backjump to: `trail.resize(asserting_level + 1)`.
    ///
    /// Not `add_asserting_clause()`'s `removed_levels`; a restart unrelates them.
    std::size_t asserting_level;

    /// @brief Every clause used, in derivation order: the conflicting clause first.
    std::vector<ClauseId> reason_ids;
};

/// @brief Derive an asserting clause from a conflicting trail, by the 1-UIP-like
/// clause learning algorithm of arXiv:2605.15002.
///
/// The LRUP(⊕) proof wants `reason_ids` reversed, which the caller does once.
///
/// @pre `trail.level() > 0`.
/// @pre The trail is conflicting: `0 = 1` is in the span, its equations are
/// reduced, and the one that made it conflicting is the most recent learned.
/// @pre Every non-decision trail row carries a reason id in `clauses`.
AssertingClause find_asserting_clause(const Trail& trail, const std::vector<Clause>& clauses,
                                      DecisionProcedure& decisions);

/// @brief One resolution step, in trail-basis coordinates: removes the coordinate
/// `pivot_column` from `asserting_basis` against the reason clause it names, then
/// merges in the reason's remaining rows.
///
/// @return The id of the reason clause resolved against.
/// @pre `asserting_basis` is non-empty and `pivot_column` leads its first row.
/// @pre `trail_basis_one` is the unit vector at column 0 of the basis.
/// @pre `pivot_column`'s trail step was propagated, with a reason id in `clauses`.
/// @pre The trail is conflicting.
/// @post `asserting_basis.row(0).leading_bit() > pivot_column`.
ClauseId resolve_asserting_basis_with_reason(TrailBasisSpace& asserting_basis,
                                             std::size_t pivot_column, const Trail& trail,
                                             const std::vector<Clause>& clauses,
                                             DecisionProcedure& decisions,
                                             const Equation& trail_basis_one);

} // namespace xorcle
