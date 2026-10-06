#pragma once

/// @file compaction.h
/// @brief Dropping the leading variables of all known units: a substitution
/// each time, and a renumbering once it shortens the equations.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "util/index_set.h"

namespace xorcle {

/// @brief One committed compaction step, enough to undo it on an assignment.
struct CompactionHistoryEntry {
    std::vector<Equation> trail_rows_before;

    /// @brief The renumbering that followed the substitution, if one did.
    std::optional<CompactionTransform> renumbering;
};

struct SubstitutionPlan {
    /// @brief The active clauses rewritten, in ascending id order: those a dropped
    /// variable occurs in or that span `0 = 1`.
    std::vector<ClauseId> rewritten_clause_ids;
    std::vector<Clause> rewritten_clauses;
    std::vector<std::uint8_t> rewritten_is_active; ///< Zero where the trail satisfied the clause.

    CompactionHistoryEntry history_entry;
};

/// @brief A read-only view of the solver state a compaction is planned against.
struct CompactionState {
    const Trail& trail;
    const std::vector<Clause>& clauses;
    /// @brief In ascending order, at least the active clauses that carry a pivot
    /// column of the trail's span or span `0 = 1`, and no inactive one.
    std::span<const ClauseId> candidate_clause_ids;

    /// @brief Per clause id, the columns an active clause's rows carry.
    const std::vector<IndexSet>& clause_carried_col_idxs;

    /// @brief The variables of the numbering, the dropped ones included.
    std::size_t active_num_vars;
};

/// @brief The one-row clause an id holds once it goes inactive.
/// @pre `false_row` is `0 = 1`.
Clause inactive_clause_placeholder(EquationRepr repr, const Equation& false_row);

/// @brief True when the trail is at the root and at least one variable can be dropped.
/// @pre The trail span's pivot column set has size `state.active_num_vars + 1`.
bool should_compact_before_decision(const CompactionState& state);

/// @brief Rewrite, reduced by the root trail, the active clauses a variable it
/// fixes occurs in; the numbering stays, and those variables' columns go to zero.
///
/// A clause the trail satisfies becomes a single inactive `0 = 1` row, so clause ids stay
/// stable; every other clause keeps its rows and its watches. Applying the plan replaces
/// the root trail with an empty one, so root-level units are not recoverable from the clause
/// database afterwards.
///
/// @pre `state.trail` is at the root level and does not span `0 = 1`, and
/// `state.clause_carried_col_idxs` is sized by clause id.
SubstitutionPlan build_substitution_plan(const CompactionState& state);

/// @brief A renumbering waits until it frees this share of each row's words, so at
/// most 5% of them are dead between renumberings.
inline constexpr std::size_t min_freed_word_percent = 5;

/// @brief Whether renumbering without the dropped variables leaves an equation
/// at least a word and at least `min_freed_word_percent` of its words shorter.
/// @pre `num_dropped_vars <= active_num_vars`.
bool renumbering_frees_enough_words(std::size_t active_num_vars, std::size_t num_dropped_vars);

} // namespace xorcle
