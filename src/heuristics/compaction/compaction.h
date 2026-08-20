#pragma once

/// @file compaction.h
/// @brief Variable compaction: eliminate the variables the root trail span
/// pivots on and rewrite the whole problem over the survivors.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/trail.h"

namespace xorcle {

/// @brief One committed compaction step, kept so an assignment found later can
/// be recovered in the pre-compaction variable numbering.
///
/// @note `trail_rows_before` is in **row-echelon order**, not trail order, which
/// is what back-substitution needs: walking it backwards solves each variable
/// after everything to its right.
struct CompactionHistoryEntry {
    CompactionTransform transform;
    std::vector<std::size_t> active_var_to_original_before;
    std::vector<Equation> trail_rows_before; ///< The root trail span, in row-echelon order.
};

/// @brief Everything one compaction produces, built before any solver state changes.
struct CompactionPlan {
    CompactionTransform transform;
    std::vector<Clause> rewritten_clauses; ///< Every clause, rewritten, in clause-id order.
    std::vector<std::uint8_t> clause_is_active; ///< The new active flags, in clause-id order.
    CompactionHistoryEntry history_entry; ///< What to record for later recovery.
};

/// @brief A read-only view of the solver state a compaction is planned against.
struct CompactionState {
    const Trail& trail; ///< The current trail; must be at the root level.
    const std::vector<Clause>& clauses; ///< The clause database, in clause-id order.
    const std::vector<std::uint8_t>& clause_is_active; ///< Current active flags.
    const std::vector<std::size_t>& active_var_to_original; ///< Active to original variable map.
    std::size_t active_num_vars;
    bool has_compacted_once; ///< Whether a compaction has already happened this run.
    std::size_t num_conflicts;
    std::size_t conflicts_at_last_compaction;
    std::size_t compaction_conflict_interval; ///< Conflicts required between compactions.
};

/// @brief True when the trail is at the root, the conflict interval has elapsed,
/// and at least one variable can actually be eliminated.
bool should_compact_before_decision(const CompactionState& state);

/// @brief Build the change of basis dropping every variable the span pivots on.
/// @pre The trail is at the root level, and its span's pivot column set has size
/// `active_num_vars + 1` and does not include the constant column.
CompactionTransform build_compaction_transform(const Trail& trail, std::size_t active_num_vars);

/// @brief Rewrite the active problem into the compacted variable numbering, as
/// one plan.
///
/// Every clause is reduced by the root trail span and then compacted. Because
/// compaction is followed by `add_row()`, which echelonizes and deduplicates, a
/// rewritten clause can have **fewer rows** than the original — which is why
/// watch metadata is rebuilt by matching row values, not by keeping indices. A
/// clause the trail satisfies becomes a single @f$\mathbf{1}@f$ row, inactive.
///
/// @warning Applying this plan **deliberately discards the root trail**: the
/// solver replaces it with an empty one over the compacted variables. That is
/// what makes root-level units unrecoverable from the clause database alone —
/// the reason proof emission and compaction are mutually exclusive. Do not try
/// to preserve the trail across a compaction.
CompactionPlan build_compaction_plan(const CompactionState& state,
                                     const CompactionTransform& transform);

} // namespace xorcle
