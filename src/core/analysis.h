#pragma once

/// @file analysis.h
/// @brief Conflict analysis: the paper's Algorithm 2, the 1-UIP-like clause
/// learning that turns a conflicting trail into an asserting clause.

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
    /// @brief The learned clause, in its linear negation.
    Clause clause;

    /// @brief The decision level the clause asserts at, i.e. the level to backjump to.
    ///
    /// This is `TrailBasisSpace::asserting_level()` of the final basis, so
    /// backjumping is `trail.resize(asserting_level + 1)` and the number of
    /// levels discarded is `trail.level() - asserting_level`.
    ///
    /// @warning This is **not** the `removed_levels` that
    /// `ClauseSelector::add_asserting_clause()` takes. Without a restart those
    /// two are complements; with one, the solver discards every non-root level
    /// regardless of where the clause asserts, and they are unrelated. The 0.1
    /// tree returned the discarded count here and called it `jumpable_levels`,
    /// which is the opposite end of the range.
    std::size_t asserting_level;
};

/// @brief Derive an asserting clause from a conflicting trail: Algorithm 2.
///
/// Starts from the clause that caused the conflict, rewritten into the trail
/// basis, and resolves against reason clauses until the result stops depending
/// on the current decision level -- the 1-UIP stopping criterion, expressed over
/// linear equations rather than literals.
///
/// @param trail The conflicting trail. Every equation it holds must be reduced,
/// and its most recent row must be @f$\mathbf{1}@f$; see the precondition below.
/// @param clauses The clause database, in clause-id order. The solver passes
/// `clause_sel->clauses()`; conflict analysis only ever reads a reason by id, so
/// it takes the database rather than the selector, exactly as
/// `DecisionProcedure::make_decision()` does.
/// @param decisions Receives the two order-sensitive notifications every
/// resolution step makes. These feed CSIDS, so their order is observable in
/// conflict counts even though it cannot affect correctness.
/// @param reason_ids If non-null, receives every clause used in the derivation,
/// **in derivation order**: the conflicting clause first, then one id per
/// resolution step. The LRUP(@f$\oplus@f$) proof wants the opposite order, so
/// the caller reverses this exactly once before logging it. Ids are appended
/// rather than replacing what the buffer already holds.
/// @return The learned clause and the level it asserts at.
///
/// @pre `trail.level() > 0`. A conflict at the root is UNSAT and is handled
/// without analysis.
/// @pre **The trail is conflicting**, i.e. @f$\mathbf{1} \in
/// \mathrm{span}(U)@f$, and the equation that made it so is the most recent one
/// learned. Everything here depends on that; see
/// `resolve_asserting_basis_with_reason()`.
/// @pre Every trail row except the per-level decisions carries a reason, and
/// every reason id is `< clauses.size()`.
/// @note All preconditions are internal invariants of the solver rather than
/// user input, so they are asserts. The 0.1 tree
/// threw `std::logic_error` from five places here.
AssertingClause find_asserting_clause(const Trail& trail, const std::vector<Clause>& clauses,
                                      DecisionProcedure& decisions,
                                      std::vector<ClauseId>* reason_ids = nullptr);

/// @brief One resolution step of Algorithm 2, in trail-basis coordinates.
///
/// Removes the coordinate `pivot_column` from `asserting_basis` by resolving its
/// first row against the reason clause of the trail step that coordinate names.
///
/// @param asserting_basis The clause being derived, in trail-basis coordinates.
/// Its first row is replaced by the resolvent and the reason clause's remaining
/// rows are merged in.
/// @param pivot_column The coordinate to resolve away. This is always
/// `asserting_basis.row(0).leading_column()`: rows are stored in ascending
/// leading-column order, so the first non-zero column of the whole matrix is the
/// first non-zero column of row 0. (The 0.1 tree scanned for it with `col()`,
/// which is the one algebra operation this rewrite does not provide.)
/// @param trail The conflicting trail.
/// @param clauses The clause database, in clause-id order.
/// @param decisions Receives `update_with_resolved_clause()` for the reason and
/// then `update_with_critical_equation()` for the trail row resolved away.
/// @param trail_basis_one The coordinate vector of @f$\mathbf{1}@f$, i.e. the
/// unit vector at column 0, of width `asserting_basis.cols()`. Passed in because
/// the resolution loop calls this repeatedly and the vector never changes within
/// one conflict; building it per step is what the 0.1 tree did.
/// @param reason_ids If non-null, receives the reason clause's id, appended.
///
/// @pre `asserting_basis.rows() > 0` and `pivot_column ==
/// asserting_basis.row(0).leading_column()`.
/// @pre `trail_basis_one` has width `asserting_basis.cols()` and column 0 as its
/// only set column.
/// @pre The trail step named by `pivot_column` was propagated, not decided.
/// @pre **The trail is conflicting.** Outside conflict analysis the identity
/// this step is built on does not hold and the result is silently wrong.
/// @post `asserting_basis.row(0).leading_column() > pivot_column`, so repeating
/// the step terminates.
void resolve_asserting_basis_with_reason(TrailBasisSpace& asserting_basis,
                                         std::size_t pivot_column, const Trail& trail,
                                         const std::vector<Clause>& clauses,
                                         DecisionProcedure& decisions,
                                         const Equation& trail_basis_one,
                                         std::vector<ClauseId>* reason_ids = nullptr);

} // namespace xorcle
