#include "heuristics/compaction/compaction.h"

#include <cassert>
#include <optional>
#include <utility>

#include "core/clause_id.h"
#include "heuristics/decisions/clause_decisions.h"

namespace xorcle {

namespace {

std::optional<std::size_t> find_row_index(const Clause& clause, const Equation& clause_row) {
    for (std::size_t row_index = 0; row_index < clause.rows(); ++row_index) {
        if (clause.row(row_index) == clause_row) {
            return row_index;
        }
    }
    return std::nullopt;
}

/// @return The first row index other than `row_to_avoid`, or 0 if there is none.
std::size_t first_distinct_row(std::size_t num_rows, std::size_t row_to_avoid) {
    for (std::size_t row_index = 0; row_index < num_rows; ++row_index) {
        if (row_index != row_to_avoid) {
            return row_index;
        }
    }
    return 0;
}

Equation compact_old_row(const Clause& old_clause, std::size_t row_index,
                         const RowSpace& trail_span, const CompactionTransform& transform) {
    assert(row_index < old_clause.rows());
    Equation reduced_row = old_clause.row(row_index);
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

    const IndexSet& pivots = state.trail.span().pivot_columns();
    assert(pivots.size() == state.active_num_vars + 1 && "trail pivot column count mismatch");
    assert(!pivots.test(state.active_num_vars) &&
           "the constant column cannot be dropped by compaction");
    return pivots.count_below(state.active_num_vars) != 0;
}

CompactionTransform build_compaction_transform(const Trail& trail, std::size_t active_num_vars) {
    const IndexSet& pivots = trail.span().pivot_columns();
    assert(pivots.size() == active_num_vars + 1 && "trail pivot column count mismatch");
    assert(!pivots.test(active_num_vars) && "the constant column cannot be dropped by compaction");
    return CompactionTransform::from_dropped_columns(active_num_vars, pivots);
}

CompactionPlan build_compaction_plan(const CompactionState& state,
                                     const CompactionTransform& transform) {
    const std::size_t num_clauses = state.clauses.size();
    assert(state.clause_is_active.size() == num_clauses &&
           "clause active flags are desynchronized");

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

        Clause rewritten(repr, transform.new_num_vars());
        if (becomes_inactive) {
            // Kept as a placeholder so clause ids stay stable; never selected.
            rewritten.add_row(false_clause_row);
            rewritten.watched0 = 0;
            rewritten.watched1 = 0;
            rewritten.next_watch_search_start = 0;
            plan.clause_is_active[clause_id] = 0;
            plan.rewritten_clauses.push_back(std::move(rewritten));
            continue;
        }

        for (std::size_t row_index = 0; row_index < active_clause.rows(); ++row_index) {
            rewritten.add_row(
                compact_old_row(active_clause, row_index, state.trail.span(), transform));
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
