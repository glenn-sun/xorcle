#pragma once

/// @file compaction.h
/// @brief Dropping the leading variables of all known units.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/trail.h"

namespace xorcle {

/// @brief One committed compaction step, enough to undo it on an assignment.
struct CompactionHistoryEntry {
    CompactionTransform transform;
    std::vector<Equation> trail_rows_before;
};

struct CompactionPlan {
    CompactionTransform transform;

    /// @brief The active clauses rewritten, in ascending id order: those a dropped
    /// variable occurs in or that span `0 = 1`. Every other active clause is left to
    /// `renumber_clause()`.
    std::vector<ClauseId> rewritten_clause_ids;
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

    /// @brief Whether a compaction is owed: before the first one, once the
    /// conflict interval has elapsed since the last, and once the root trail has
    /// gained units the clause database has not been rewritten against.
    bool is_due;
};

/// @brief The one-row clause an id holds once it goes inactive.
/// @pre `false_row` is `0 = 1`.
Clause inactive_clause_placeholder(EquationRepr repr, const Equation& false_row);

/// @brief True when a compaction is due, the trail is at the root, and at least
/// one variable can be dropped.
/// @pre The trail span's pivot column set has size `state.active_num_vars + 1`.
bool should_compact_before_decision(const CompactionState& state);

/// @brief Build the renumbering dropping the leading variables of all known units.
/// @pre The trail is at the root level, and its span's pivot column set has size
/// `active_num_vars + 1` and does not include the constant column.
CompactionTransform build_compaction_transform(const Trail& trail, std::size_t active_num_vars);

/// @brief Rewrite into the compacted numbering the active clauses a dropped variable occurs in.
///
/// A clause the trail satisfies becomes a single inactive `0 = 1` row, so clause ids stay
/// stable; an id already inactive is left to whatever it holds. Applying the plan replaces
/// the root trail with an empty one, so root-level units are not recoverable from the clause
/// database afterwards.
///
/// @pre `state.trail` is at the root level, `state.clause_is_active` and
/// `state.active_var_to_original` are sized by clause id and by active variable, and
/// `transform.old_num_vars() == state.active_num_vars`.
CompactionPlan build_compaction_plan(const CompactionState& state,
                                     const CompactionTransform& transform);

/// @brief Renumber in place an active clause the plan does not rewrite.
/// @pre No dropped variable occurs in `clause`, and it does not span `0 = 1`.
void renumber_clause(Clause& clause, const CompactionTransform& transform);

} // namespace xorcle
