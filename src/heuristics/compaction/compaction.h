#pragma once

/// @file compaction.h
/// @brief Eliminating the variables the root trail span pivots on.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/trail.h"

namespace xorcle {

/// @brief One committed compaction step, enough to undo it on an assignment.
struct CompactionHistoryEntry {
    CompactionTransform transform;
    std::vector<std::size_t> active_var_to_original_before;
    std::vector<Equation> trail_rows_before;
};

struct CompactionPlan {
    CompactionTransform transform;
    std::vector<Clause> rewritten_clauses;
    std::vector<std::uint8_t> clause_is_active;
    CompactionHistoryEntry history_entry;
};

/// @brief A read-only view of the solver state a compaction is planned against.
struct CompactionState {
    const Trail& trail;
    const std::vector<Clause>& clauses;
    const std::vector<std::uint8_t>& clause_is_active;
    const std::vector<std::size_t>& active_var_to_original;
    std::size_t active_num_vars;
    bool has_compacted_once;
    std::size_t num_conflicts;
    std::size_t conflicts_at_last_compaction;
    std::size_t compaction_conflict_interval;
};

/// @brief True when the trail is at the root, the conflict interval has elapsed,
/// and at least one variable can be eliminated.
/// @pre The trail span's pivot column set has size `state.active_num_vars + 1`.
bool should_compact_before_decision(const CompactionState& state);

/// @brief Build the change of basis dropping every variable the span pivots on.
/// @pre The trail is at the root level, and its span's pivot column set has size
/// `active_num_vars + 1` and does not include the constant column.
CompactionTransform build_compaction_transform(const Trail& trail, std::size_t active_num_vars);

/// @brief Rewrite the active problem into the compacted numbering, in clause-id order.
///
/// A clause the trail satisfies becomes a single inactive `0 = 1` row, so clause ids stay
/// stable. Applying the plan replaces the root trail with an empty one, so root-level units
/// are not recoverable from the clause database afterwards.
///
/// @pre `state.trail` is at the root level, `state.clause_is_active` and
/// `state.active_var_to_original` are sized by clause id and by active variable, and
/// `transform.old_num_vars() == state.active_num_vars`.
CompactionPlan build_compaction_plan(const CompactionState& state,
                                     const CompactionTransform& transform);

} // namespace xorcle
