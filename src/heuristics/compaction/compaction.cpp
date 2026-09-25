#include "heuristics/compaction/compaction.h"

#include <cassert>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "core/clause_id.h"
#include "heuristics/decisions/clause_decisions.h"

namespace xorcle {

namespace {

/// @brief Re-establish a rewritten clause's watches and cyclic search cursor.
///
/// Applying the plan leaves an empty root trail, against which no stored row is
/// `0 = 0` and any two distinct rows are independent, so the first two rows are
/// a legal pair for every clause wider than a unit.
void rebuild_clause_watch_metadata(Clause& rewritten) {
    rewritten.watched0 = 0;
    rewritten.watched1 = rewritten.rows() <= 1 ? 0 : 1;
    rewritten.next_watch_search_start = 0;
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
    if (!state.is_due) {
        return false;
    }
    if (state.trail.level() != 0) {
        return false; // compaction is only sound against the root trail
    }
    if (state.trail.span().rows() < 1) {
        return false;
    }
    const IndexSet& pivots = state.trail.span().pivot_col_idxs();
    assert(pivots.size() == state.active_num_vars + 1 && "trail pivot column count mismatch");
    return pivots.count_below(state.active_num_vars) != 0;
}

CompactionTransform build_compaction_transform(const Trail& trail, std::size_t active_num_vars) {
    assert(trail.level() == 0 && "compaction is only sound against the root trail");
    // The pivot set's size and its constant column are checked one layer down, by
    // `CompactionTransform::from_dropped_columns()`.
    return CompactionTransform::from_dropped_columns(active_num_vars,
                                                     trail.span().pivot_col_idxs());
}

CompactionPlan build_compaction_plan(const CompactionState& state,
                                     const CompactionTransform& transform) {
    const std::size_t num_clauses = state.clauses.size();
    assert(state.trail.level() == 0 && "compaction is only sound against the root trail");
    assert(state.clause_is_active.size() == num_clauses &&
           "clause active flags are desynchronized");
    assert(state.active_var_to_original.size() == state.active_num_vars &&
           "the active-to-original variable map is desynchronized");
    assert(transform.old_num_vars() == state.active_num_vars &&
           "the transform was built for a different active variable numbering");

    CompactionPlan plan;
    plan.transform = transform;
    plan.clause_is_active = state.clause_is_active;

    const EquationRepr repr = state.trail.repr();
    // Reduced rows are the same whatever basis reduces them, and a reduced
    // basis reduces each pivot column with one addition.
    RowSpace root_span = state.trail.span();
    root_span.reduce_rows();
    ClauseSatisfactionWorkspace satisfaction_workspace(repr, state.active_num_vars);
    std::vector<Equation> reduced_rows;
    const Equation false_clause_row = Equation::eq_false(repr, transform.new_num_vars());
    // The dropped variables are the trail span's pivot columns. Testing each in a
    // row beats a word-wise test while there are fewer of them than words in a row.
    const IndexSet& dropped_var_set = state.trail.span().pivot_col_idxs();
    const bool tests_each_dropped_var =
        state.trail.span().rows() < (state.active_num_vars + 1 + 63) / 64;
    std::vector<std::size_t> dropped_vars;
    if (tests_each_dropped_var) {
        dropped_var_set.for_each_set([&](std::size_t var) { dropped_vars.push_back(var); });
    }
    const std::size_t constant_col_idx = state.active_num_vars;

    for (ClauseId clause_id = 0; clause_id < num_clauses; ++clause_id) {
        if (state.clause_is_active[clause_id] == 0) {
            continue;
        }
        const Clause& active_clause = state.clauses[clause_id];
        // A clause no dropped variable occurs in is its own reduction by the trail,
        // which then satisfies it only if it already spans `0 = 1`.
        if (!active_clause.pivot_col_idxs().test(constant_col_idx) &&
            !(tests_each_dropped_var ? active_clause.carries_any_col(dropped_vars)
                                     : active_clause.carries_any_col(dropped_var_set))) {
            continue;
        }
        plan.rewritten_clause_ids.push_back(clause_id);
        if (clause_status_by_trail(root_span, active_clause, satisfaction_workspace,
                                   &reduced_rows) == ClauseStatus::satisfied) {
            // Kept as a placeholder so clause ids stay stable; never selected.
            plan.clause_is_active[clause_id] = 0;
            plan.rewritten_clauses.push_back(inactive_clause_placeholder(repr, false_clause_row));
            continue;
        }

        assert(reduced_rows.size() == active_clause.rows() &&
               "the satisfaction test did not reduce every row");
        Clause rewritten(repr, transform.new_num_vars());
        for (const Equation& reduced_row : reduced_rows) {
            rewritten.add_row(reduced_row.compact(transform));
        }
        assert(rewritten.rows() != 0 && "an active clause compacted to zero rows");

        rebuild_clause_watch_metadata(rewritten);
        plan.rewritten_clauses.push_back(std::move(rewritten));
    }

    plan.history_entry.transform = transform;
    plan.history_entry.trail_rows_before.reserve(state.trail.span().rows());
    for (const Equation& row : state.trail.span()) {
        plan.history_entry.trail_rows_before.push_back(row);
    }
    return plan;
}

void renumber_clause(Clause& clause, const CompactionTransform& transform) {
    assert(!clause.pivot_col_idxs().test(clause.num_vars()) && "the clause spans 0 = 1");
#ifndef NDEBUG
    for (const Equation& row : clause) {
        for (const std::size_t bit : row.bits()) {
            assert((bit == clause.num_vars() ||
                    transform.to_new(bit) != CompactionTransform::npos) &&
                   "a dropped variable occurs in the clause");
        }
    }
#endif
    clause.compact(transform);
    rebuild_clause_watch_metadata(clause);
    clause.satisfied_level = 0;
    clause.satisfied_stamp = 0;
}

} // namespace xorcle
