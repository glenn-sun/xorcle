#pragma once

#include <cstddef>
#include <optional>
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

    /// @brief Each `reason_ids` entry's literal block distance on this trail, or
    /// empty when none were measured.
    std::vector<std::size_t> reason_lbds;

    /// @brief How many decision levels the clause reaches into, measured on the
    /// trail it was derived from.
    std::size_t lbd;

    /// @brief The clause's dimension when it first became asserting, which is
    /// `clause.rows()` unless minimization shrank it further.
    std::size_t dim_before_minimize;
};

/// @brief Derive an asserting clause from a conflicting trail, by the 1-UIP-like
/// clause learning algorithm of arXiv:2605.15002.
///
/// With `minimize`, the walk does not stop at the asserting point but runs to
/// the beginning of the trail, taking only the resolutions that strictly shrink
/// the clause's dimension.
///
/// With `measure_reason_lbds`, every clause the derivation uses is measured and
/// fills `reason_lbds`.
///
/// The LRUP(⊕) proof wants `reason_ids` reversed, which the caller does once.
///
/// @pre `trail.level() > 0`.
/// @pre The trail is conflicting: `0 = 1` is in the span, its equations are
/// reduced, and the one that made it conflicting is the most recent learned.
/// @pre Every non-decision trail row carries a reason id in `clauses`.
AssertingClause find_asserting_clause(const Trail& trail, const std::vector<Clause>& clauses,
                                      DecisionProcedure& decisions, bool minimize = true,
                                      bool measure_reason_lbds = false);

/// @brief The clauses a root conflict actually depends on, in replay order.
///
/// The same walk as `find_asserting_clause()` at level 0, with no asserting
/// point to stop at: it runs until nothing is left to resolve, which is the
/// empty clause. Root propagations the conflict does not use are left out.
///
/// @pre `trail.level() == 0` and the trail is conflicting.
/// @pre Every trail row carries a reason id in `clauses`, which the root does.
std::vector<ClauseId> find_root_conflict_reasons(const Trail& trail,
                                                 const std::vector<Clause>& clauses);

/// @brief The reason clause a resolution step used.
struct ResolvedReason {
    ClauseId clause_id;

    /// @brief The clause's literal block distance on this trail, when the step
    /// was asked to measure it.
    std::optional<std::size_t> lbd;
};

/// @brief One resolution step, in trail-basis coordinates: removes the coordinate
/// `pivot_column` from `asserting_basis` against the reason clause it names, then
/// merges in the reason's remaining rows.
///
/// @return The reason clause resolved against, measured when `measure_lbd`.
/// @pre `asserting_basis` is non-empty and `pivot_column` leads its first row.
/// @pre `trail_basis_one` is the unit vector at column 0 of the basis.
/// @pre `pivot_column`'s trail step was propagated, with a reason id in `clauses`.
/// @pre The trail is conflicting.
/// @post `asserting_basis` is non-empty and its first row's leading column is
/// past `pivot_column`.
ResolvedReason resolve_asserting_basis_with_reason(TrailBasisSpace& asserting_basis,
                                                   std::size_t pivot_col_idx, const Trail& trail,
                                                   const std::vector<Clause>& clauses,
                                                   DecisionProcedure& decisions,
                                                   const Equation& trail_basis_one,
                                                   bool measure_lbd = false);

/// @brief The same resolution step on any row, leaving the decision procedure
/// untouched.
///
/// Continued past the asserting point the step can leave nothing after the pivot
/// row, which is why this one does not require a row there afterwards.
///
/// @return The reason clause resolved against, measured when `measure_lbd`.
/// @pre `pivot_col_idx` leads row `pivot_row_idx`; otherwise as
/// `resolve_asserting_basis_with_reason()`.
/// @post No row from `pivot_row_idx` on leads at or before `pivot_column`.
ResolvedReason resolve_asserting_basis(TrailBasisSpace& asserting_basis, std::size_t pivot_row_idx,
                                       std::size_t pivot_col_idx, const Trail& trail,
                                       const std::vector<Clause>& clauses,
                                       const Equation& trail_basis_one, bool measure_lbd = false);

} // namespace xorcle
