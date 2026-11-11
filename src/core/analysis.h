#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_pool.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "core/trail_basis.h"
#include "util/random.h"

namespace xorcle {

/// @brief A reason clause some resolvent of the walk subsumed.
struct SubsumedReason {
    ClauseId clause_id; ///< The subsumed reason clause.

    /// @brief The resolvent at that step, one dimension smaller than the reason,
    /// or empty when it is the clause as learned, which the caller already has.
    std::optional<Clause> replacement;

    std::size_t lbd; ///< The replacement's literal block distance on this trail.

    /// @brief The replacement's derivation chain length: the first
    /// `num_reasons - 1` entries of `reason_ids` followed by `clause_id` itself,
    /// which for a resolution the walk did not accept is not in `reason_ids`.
    std::size_t num_reasons;

    /// @brief The trail row the reason propagated: after the backjump the trail
    /// still names the reason exactly if this row survived.
    std::size_t trail_idx;

    /// @brief Whether the subsumed clause is the conflicting clause.
    ///
    /// It opens the derivation instead of closing it, so the replacement's chain
    /// is the first `num_reasons` entries of `reason_ids` with nothing appended.
    bool subsumed_conflicting_clause = false;
};

/// @brief What conflict analysis derives: a clause and the level it asserts at.
struct AssertingClause {
    Clause clause; ///< In its linear negation.

    /// @brief The level to backjump to: `trail.resize(asserting_level + 1)`.
    ///
    /// Not `add_asserting_clause()`'s `removed_levels`; a restart unrelates them.
    std::size_t asserting_level;

    /// @brief Every clause used, in derivation order: the conflicting clause first.
    std::vector<ClauseId> reason_ids;

    /// @brief Each `reason_ids` entry's literal block distance on this trail.
    std::vector<std::size_t> reason_lbds;

    /// @brief How many decision levels above the root the clause reaches into,
    /// measured on the trail it was derived from.
    std::size_t lbd;

    /// @brief The clause's width when it first became asserting, which is
    /// `clause.rows()` unless minimization shrank it further.
    std::size_t width_before_minimize;

    /// @brief The trail row of each pivot of the clause, ascending by pivot
    /// column, or empty when none were asked for.
    ///
    /// Reading the trail as a flag of subspaces, these are the rows at which the
    /// intersection with the clause's subspace gains a dimension.
    std::vector<std::size_t> responsible_trail_idxs;

    /// @brief Every reason clause a resolvent subsumed, in derivation order, or
    /// empty when the walk was not asked to collect them.
    std::vector<SubsumedReason> subsumed_reasons;
};

/// @brief Derive an asserting clause from a conflicting trail, by the 1-UIP-like
/// clause learning algorithm of arXiv:2605.15002.
///
/// With `minimize`, the walk does not stop at the asserting point but runs to
/// the beginning of the trail, taking only the resolutions that strictly shrink
/// the clause's width.
///
/// With `report_responsible_trail_idxs`, the trail elements responsible for the
/// clause as learned fill `responsible_trail_idxs`.
///
/// With `collect_subsumed`, every resolution whose resolvent subsumes its
/// reason clause records the resolvent in `subsumed_reasons`, and so does the
/// first accepted one when it subsumes the conflicting clause.
///
/// `representative_rng` makes each minimization step pick its resolvent
/// uniformly at random from the family the step leaves free; see
/// `resolve_asserting_basis()`.
///
/// The LRUP(⊕) proof wants `reason_ids` reversed, which the caller does once.
///
/// @pre `trail.level() > 0`.
/// @pre The trail is conflicting: `0 = 1` is in the span, its equations are
/// reduced, and `conflict_clause_id` propagated the one that made it so, which
/// is the last in trail order.
/// @pre Every non-decision trail row carries a reason id in `clauses`.
AssertingClause find_asserting_clause(const Trail& trail, const std::vector<Clause>& clauses,
                                      ClauseId conflict_clause_id, Rng& representative_rng,
                                      bool minimize = true,
                                      bool report_responsible_trail_idxs = false,
                                      bool collect_subsumed = false);

/// @brief The clauses a root conflict actually depends on, in replay order.
///
/// The same walk as `find_asserting_clause()` at level 0, with no asserting
/// point to stop at: it runs until nothing is left to resolve, which is the
/// empty clause. Root propagations the conflict does not use are left out.
///
/// @pre `trail.level() == 0`, the trail is conflicting, and
/// `conflict_clause_id` propagated the row that made it so.
/// @pre Every trail row carries a reason id in `clauses`, which the root does.
std::vector<ClauseId> find_root_conflict_reasons(const Trail& trail,
                                                 const std::vector<Clause>& clauses,
                                                 ClauseId conflict_clause_id,
                                                 Rng& representative_rng);

/// @brief The reason clause a resolution step used.
struct ResolvedReason {
    ClauseId clause_id;

    /// @brief The clause's literal block distance on this trail, with the trail
    /// equation it propagated counted at that equation's level.
    std::size_t lbd;

    /// @brief Whether the resolvent's rank dropped to the reason's less one, so
    /// its span is exactly the reason's remaining rows': the resolvent subsumes
    /// the reason clause.
    bool subsumes_reason = false;
};

/// @brief What the resolution steps on one conflicting trail share.
struct ResolutionWorkspace {
    /// @pre The trail is conflicting, and stays as it is while this is used.
    explicit ResolutionWorkspace(const Trail& trail);

    Equation trail_basis_one; ///< The unit vector at column 0, which is `0 = 1`.
    TrailBasisSpace reason_basis; ///< Empty between steps.
    EquationPool spare_rows; ///< Trail-basis rows a step was left with.
};

/// @brief One resolution step, in trail-basis coordinates: removes the coordinate
/// `pivot_col_idx` from row `pivot_row_idx` of `asserting_basis` against the reason
/// clause it names, then merges in the reason's remaining rows.
///
/// Continued past the asserting point the step can leave nothing after the pivot
/// row.
///
/// With `pivot_row_idx > 0` the rows before the pivot are free: each may take
/// the popped pivot row or not, one choice per row, and every combination spans
/// a valid resolvent. A fair coin from `representative_rng` per such row draws
/// the resolvent uniformly from that family.
///
/// @return The reason clause resolved against, with its LBD.
/// @pre `pivot_col_idx` leads row `pivot_row_idx`, its trail step was propagated
/// with a reason id in `clauses`, and the trail is conflicting.
/// @pre `workspace` was built on `trail`.
/// @post No row from `pivot_row_idx` on leads at or before `pivot_col_idx`.
ResolvedReason resolve_asserting_basis(
    TrailBasisSpace& asserting_basis, std::size_t pivot_row_idx, std::size_t pivot_col_idx,
    const Trail& trail, const std::vector<Clause>& clauses, ResolutionWorkspace& workspace,
    Rng& representative_rng);

} // namespace xorcle
