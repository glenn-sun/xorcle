#pragma once

/// @file compaction.h
/// @brief Variable compaction: eliminate the variables the root trail has
/// already pivoted on, and rewrite the whole problem into the smaller space.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/trail.h"

namespace xorcle {

/// @brief One committed compaction step, kept so an assignment found later can
/// be lifted back out of the compacted space.
///
/// @note `trail_rows_before` is stored in **row-echelon order**, not trail
/// order. That is exactly the order back-substitution needs: leading columns
/// increase along it, so walking it backwards solves each eliminated variable
/// only after everything to its right is known.
struct CompactionHistoryEntry {
    CompactionTransform transform; ///< The change of basis this step applied.
    std::vector<std::size_t> active_var_to_original_before; ///< The variable map before the step.
    std::vector<Equation> trail_rows_before; ///< The root trail span, in row-echelon order.
};

/// @brief Everything one compaction produces, built before any solver state changes.
///
/// @note The 0.1 tree also carried `proof_assumptions` here — the root-level
/// units that `apply_compaction` discards, lifted into original space so later
/// proof lines could name them. Proof emission with compaction on is not ported,
/// so that field is gone; `history_entry` stays,
/// because recovering an original-space assignment still needs it.
struct CompactionPlan {
    CompactionTransform transform; ///< The change of basis being applied.
    std::vector<Clause> rewritten_clauses; ///< Every clause, rewritten, in clause-id order.
    std::vector<std::uint8_t> clause_is_active; ///< The new active flags, in clause-id order.
    CompactionHistoryEntry history_entry; ///< What to record for later lifting.
};

/// @brief A read-only view of the solver state a compaction is planned against.
struct CompactionInputs {
    const Trail& trail; ///< The current trail; must be at the root level.
    const std::vector<Clause>& clauses; ///< The clause database, in clause-id order.
    const std::vector<std::uint8_t>& clause_is_active; ///< Current active flags.
    const std::vector<std::size_t>& active_var_to_original; ///< Active to original variable map.
    std::size_t active_num_vars; ///< Number of active variable columns.
    bool has_compacted_once; ///< Whether a compaction has already happened this run.
    std::size_t num_conflicts; ///< Conflicts so far.
    std::size_t conflicts_at_last_compaction; ///< Conflict count at the previous compaction.
    std::size_t compaction_conflict_interval; ///< Conflicts required between compactions.
};

/// @brief Whether compaction should run before the next decision.
/// @param inputs The current solver state.
/// @return True when the trail is at the root, the conflict interval has
/// elapsed, and at least one variable can actually be eliminated.
bool should_compact_before_decision(const CompactionInputs& inputs);

/// @brief Build the change of basis from the root trail's pivot columns.
/// @param trail The current trail, at the root level.
/// @param active_num_vars Number of active variable columns.
/// @return The transform eliminating every pivoted variable.
/// @pre The trail span's pivot columns have width `active_num_vars + 1` and do
/// not include the constant column.
CompactionTransform build_compaction_transform(const Trail& trail, std::size_t active_num_vars);

/// @brief Rewrite the whole active problem into the compacted variable space.
/// @param inputs The current solver state.
/// @param transform The change of basis to apply.
/// @return A plan the solver can apply in one step.
///
/// Every clause is reduced by the root trail span and then projected. Because
/// projection is followed by `add_row()`, which echelonizes and deduplicates, a
/// rewritten clause can have **fewer rows** than the original — which is why the
/// watch metadata has to be rebuilt by matching row values rather than by
/// keeping indices. A clause the trail already satisfies becomes a single
/// @f$\mathbf{1}@f$ row and is marked inactive.
///
/// @warning Applying this plan discards the root trail: the solver replaces it
/// with an empty one over the compacted variables. That is deliberate, and it
/// is what makes every root-level unit unrecoverable from the clause database
/// alone — the reason proofs and compaction are mutually exclusive in 0.2. Do
/// not try to preserve the trail across a compaction.
CompactionPlan build_compaction_plan(const CompactionInputs& inputs,
                                     const CompactionTransform& transform);

} // namespace xorcle
