#include "heuristics/compaction/compaction.h"

#include <cassert>
#include <optional>
#include <utility>

#include "core/clause_id.h"
#include "heuristics/decisions/clause_decisions.h"

namespace xorcle {

namespace {

std::optional<std::size_t> find_row_index(const Clause& clause, const Equation& clause_row) {
    for (std::size_t row_idx = 0; row_idx < clause.rows(); ++row_idx) {
        if (clause.row(row_idx) == clause_row) {
            return row_idx;
        }
    }
    return std::nullopt;
}

/// @return The first row index other than `row_to_avoid`, or 0 if there is none.
std::size_t first_distinct_row(std::size_t num_rows, std::size_t row_to_avoid) {
    for (std::size_t row_idx = 0; row_idx < num_rows; ++row_idx) {
        if (row_idx != row_to_avoid) {
            return row_idx;
        }
    }
    return 0;
}

Equation compact_old_row(const Clause& old_clause, std::size_t row_idx,
                         const RowSpace& trail_span, const CompactionTransform& transform) {
    Equation reduced_row = old_clause.row(row_idx);
    trail_span.reduce(reduced_row);
    return reduced_row.compact(transform);
}

/// @brief Re-establish a rewritten clause's watches and cyclic search cursor.
///
/// Each old watch is compacted and looked up by value; whichever lookups fail
/// fall back to any legal pair. `next_watch_search_start` is persistent
/// per-clause state, so it is carried across modulo the new row count.
void rebuild_clause_watch_metadata(Clause& rewritten, const Clause& old_clause,
                                   const RowSpace& trail_span,
                                   const CompactionTransform& transform) {
    if (rewritten.rows() <= 1) {
        rewritten.watched0 = 0;
        rewritten.watched1 = 0;
        rewritten.next_watch_search_start = 0;
        return;
    }

    const std::optional<std::size_t> mapped_watched0 = find_row_index(
        rewritten, compact_old_row(old_clause, old_clause.watched0, trail_span, transform));
    const std::optional<std::size_t> mapped_watched1 = find_row_index(
        rewritten, compact_old_row(old_clause, old_clause.watched1, trail_span, transform));

    std::size_t watched0 = mapped_watched0.value_or(mapped_watched1.value_or(0));
    if (watched0 >= rewritten.rows()) {
        watched0 = 0;
    }

    std::size_t watched1 = mapped_watched1.value_or(first_distinct_row(rewritten.rows(), watched0));
    if (watched1 >= rewritten.rows() || watched1 == watched0) {
        watched1 = first_distinct_row(rewritten.rows(), watched0);
    }

    rewritten.watched0 = watched0;
    rewritten.watched1 = watched1;
    rewritten.next_watch_search_start = old_clause.next_watch_search_start % rewritten.rows();
}

} // namespace

Clause inactive_clause_placeholder(EquationRepr repr, const Equation& false_row) {
    assert(!false_row.is_null() && false_row.is_eq_false() && "the placeholder row is not 0 = 1");
    Clause placeholder(repr, false_row.num_bits() - 1);
    placeholder.add_row(false_row);
    placeholder.watched0 = 0;
    placeholder.watched1 = 0;
    placeholder.next_watch_search_start = 0;
    return placeholder;
}

bool should_compact_before_decision(const CompactionState& state) {
    if (state.trail.size() != 1) {
        return false; // compaction is only sound against the root trail
    }
    if (state.trail.span().rows() < 1) {
        return false;
    }
    if (state.has_compacted_once) {
        const std::size_t conflicts_since =
            state.num_conflicts - state.conflicts_at_last_compaction;
        if (conflicts_since < state.compaction_conflict_interval) {
            return false;
        }
    }
    if (state.active_num_vars == 0) {
        return false;
    }

    const IndexSet& pivots = state.trail.span().pivot_col_idxs();
    assert(pivots.size() == state.active_num_vars + 1 && "trail pivot column count mismatch");
    return pivots.count_below(state.active_num_vars) != 0;
}

CompactionTransform build_compaction_transform(const Trail& trail, std::size_t active_num_vars) {
    assert(trail.size() == 1 && "compaction is only sound against the root trail");
    // The pivot set's size and its constant column are checked one layer down, by
    // `CompactionTransform::from_dropped_columns()`.
    return CompactionTransform::from_dropped_columns(active_num_vars,
                                                     trail.span().pivot_col_idxs());
}

CompactionPlan build_compaction_plan(const CompactionState& state,
                                     const CompactionTransform& transform) {
    const std::size_t num_clauses = state.clauses.size();
    assert(state.trail.size() == 1 && "compaction is only sound against the root trail");
    assert(state.clause_is_active.size() == num_clauses &&
           "clause active flags are desynchronized");
    assert(state.active_var_to_original.size() == state.active_num_vars &&
           "the active-to-original variable map is desynchronized");
    assert(transform.old_num_vars() == state.active_num_vars &&
           "the transform was built for a different active variable numbering");

    CompactionPlan plan;
    plan.transform = transform;
    plan.rewritten_clauses.reserve(num_clauses);
    plan.clause_is_active = state.clause_is_active;

    const EquationRepr repr = state.trail.repr();
    ClauseSatisfactionWorkspace satisfaction_workspace(repr, state.active_num_vars);
    const Equation false_clause_row = Equation::eq_false(repr, transform.new_num_vars());

    for (ClauseId clause_id = 0; clause_id < num_clauses; ++clause_id) {
        const Clause& active_clause = state.clauses[clause_id];
        const bool was_active = state.clause_is_active[clause_id] != 0;
        const bool becomes_inactive =
            !was_active || clause_satisfied_by_trail(state.trail.span(), active_clause,
                                                     satisfaction_workspace);

        if (becomes_inactive) {
            // Kept as a placeholder so clause ids stay stable; never selected.
            plan.clause_is_active[clause_id] = 0;
            plan.rewritten_clauses.push_back(inactive_clause_placeholder(repr, false_clause_row));
            continue;
        }

        Clause rewritten(repr, transform.new_num_vars());
        for (std::size_t row_idx = 0; row_idx < active_clause.rows(); ++row_idx) {
            rewritten.add_row(
                compact_old_row(active_clause, row_idx, state.trail.span(), transform));
        }
        assert(rewritten.rows() != 0 && "an active clause compacted to zero rows");

        rebuild_clause_watch_metadata(rewritten, active_clause, state.trail.span(), transform);
        plan.clause_is_active[clause_id] = 1;
        plan.rewritten_clauses.push_back(std::move(rewritten));
    }

    plan.history_entry.transform = transform;
    plan.history_entry.active_var_to_original_before = state.active_var_to_original;
    plan.history_entry.trail_rows_before.reserve(state.trail.span().rows());
    for (const Equation& row : state.trail.span()) {
        plan.history_entry.trail_rows_before.push_back(row);
    }
    return plan;
}

} // namespace xorcle
