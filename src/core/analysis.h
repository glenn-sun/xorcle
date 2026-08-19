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

    /// @brief The level the clause asserts at, so backjumping is
    /// `trail.resize(asserting_level + 1)` and the count of levels discarded is
    /// `trail.level() - asserting_level`.
    ///
    /// @warning This is **not** the `removed_levels` that
    /// `ClauseSelector::add_asserting_clause()` takes. Without a restart those
    /// two are complements; with one, the solver discards every non-root level
    /// regardless of where the clause asserts, and they are unrelated.
    std::size_t asserting_level;
};

/// @brief Derive an asserting clause from a conflicting trail: start from the
/// clause that caused the conflict, rewritten into the trail basis, and resolve
/// against reason clauses until the result stops depending on the current
/// decision level -- the 1-UIP stopping criterion, over linear equations rather
/// than literals. `clauses` is indexed by clause id.
///
/// `reason_ids`, if non-null, receives every clause used in the derivation,
/// **in derivation order**: the conflicting clause first, then one id per
/// resolution step. The LRUP(@f$\oplus@f$) proof wants the opposite order, so
/// the caller reverses this exactly once before logging it. Ids are appended
/// rather than replacing what the buffer already holds.
///
/// @pre `trail.level() > 0`; a root conflict is UNSAT and is not analysed.
/// @pre **The trail is conflicting**, i.e. @f$\mathbf{1} \in
/// \mathrm{span}(U)@f$, every equation it holds is reduced, and the equation
/// that made it conflicting is the most recent one learned. Everything here
/// depends on that; see `resolve_asserting_basis_with_reason()`.
/// @pre Every trail row except the per-level decisions carries a reason, and
/// every reason id is `< clauses.size()`.
AssertingClause find_asserting_clause(const Trail& trail, const std::vector<Clause>& clauses,
                                      DecisionProcedure& decisions,
                                      std::vector<ClauseId>* reason_ids = nullptr);

/// @brief One resolution step, in trail-basis coordinates: removes the
/// coordinate `pivot_column` from `asserting_basis` by resolving its first row
/// against the reason clause of the trail step that coordinate names, then
/// merging in the reason's remaining rows.
///
/// `pivot_column` is always `asserting_basis.row(0).leading_bit()`: rows are
/// stored in ascending leading-column order, so the matrix's first non-zero
/// column is row 0's. `trail_basis_one` is the coordinate vector of
/// @f$\mathbf{1}@f$, the unit vector at column 0, passed in because the loop
/// calls this repeatedly and it never changes within one conflict. `reason_ids`,
/// if non-null, receives the reason clause's id, appended.
///
/// @pre `asserting_basis.rows() > 0` and `pivot_column ==
/// asserting_basis.row(0).leading_bit()`.
/// @pre The trail step named by `pivot_column` was propagated, not decided.
/// @pre **The trail is conflicting.** Outside conflict analysis the identity
/// this step is built on does not hold and the result is silently wrong.
/// @post `asserting_basis.row(0).leading_bit() > pivot_column`, so repeating
/// the step terminates.
void resolve_asserting_basis_with_reason(TrailBasisSpace& asserting_basis,
                                         std::size_t pivot_column, const Trail& trail,
                                         const std::vector<Clause>& clauses,
                                         DecisionProcedure& decisions,
                                         const Equation& trail_basis_one,
                                         std::vector<ClauseId>* reason_ids = nullptr);

} // namespace xorcle
