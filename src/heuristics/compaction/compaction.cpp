#include "heuristics/compaction/compaction.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

#include "algebra/dense/dense_gf2_vector.h"
#include "core/clause_id.h"
#include "heuristics/decisions/clause_decisions.h"

namespace xorcle {

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

SubstitutionPlan build_substitution_plan(const CompactionState& state) {
    const std::size_t constant_col_idx = state.active_num_vars;
    assert(state.trail.level() == 0 && "compaction is only sound against the root trail");
    assert(!state.trail.span().pivot_col_idxs().test(constant_col_idx) &&
           "the root trail spans 0 = 1");
    assert(state.clause_carried_col_idxs.size() == state.clauses.size() &&
           "the clauses' carried columns are desynchronized");

    SubstitutionPlan plan;

    const EquationRepr repr = state.trail.repr();
    // Reduced rows are the same whatever basis reduces them, and a reduced
    // basis reduces each pivot column with one addition.
    RowSpace root_span = state.trail.span();
    root_span.reduce_rows();
    ClauseSatisfactionWorkspace satisfaction_workspace(repr, state.active_num_vars);
    std::vector<Equation> reduced_rows;
    const Equation false_clause_row = Equation::eq_false(repr, state.active_num_vars);
    // The dropped variables are the trail span's pivot columns. Testing each
    // beats a word-wise test while there are fewer of them than words.
    const IndexSet& dropped_var_set = state.trail.span().pivot_col_idxs();
    const bool tests_each_dropped_var =
        state.trail.span().rows() < (state.active_num_vars + DenseGF2Vector::bits_per_word) /
                                        DenseGF2Vector::bits_per_word;
    std::vector<std::size_t> dropped_vars;
    if (tests_each_dropped_var) {
        dropped_var_set.for_each_set([&](std::size_t var) { dropped_vars.push_back(var); });
    }
    const auto carries_a_dropped_var = [&](const IndexSet& carried) {
        if (!tests_each_dropped_var) {
            return carried.intersects(dropped_var_set);
        }
        return std::ranges::any_of(dropped_vars,
                                   [&](std::size_t var) { return carried.test(var); });
    };

    assert(std::ranges::is_sorted(state.candidate_clause_ids) &&
           "the candidate clauses are not in ascending order");
    for (const ClauseId clause_id : state.candidate_clause_ids) {
        const Clause& active_clause = state.clauses[clause_id];
        assert(state.clause_carried_col_idxs[clause_id] == active_clause.carried_col_idxs() &&
               "a clause's carried columns are stale");
        // A clause no dropped variable occurs in is its own reduction by the trail,
        // which then satisfies it only if it already spans `0 = 1`.
        if (!active_clause.pivot_col_idxs().test(constant_col_idx) &&
            !carries_a_dropped_var(state.clause_carried_col_idxs[clause_id])) {
            continue;
        }
        plan.rewritten_clause_ids.push_back(clause_id);
        if (clause_status_by_trail(root_span, active_clause, satisfaction_workspace,
                                   &reduced_rows) == ClauseStatus::satisfied) {
            // Kept as a placeholder so clause ids stay stable; never selected.
            plan.rewritten_is_active.push_back(0);
            plan.rewritten_clauses.push_back(inactive_clause_placeholder(repr, false_clause_row));
            continue;
        }

        assert(reduced_rows.size() == active_clause.rows() &&
               "the satisfaction test did not reduce every row");
        Clause rewritten(repr, state.active_num_vars);
        for (const Equation& reduced_row : reduced_rows) {
            rewritten.add_row(reduced_row);
        }
        assert(rewritten.rows() != 0 && "an active clause was rewritten to zero rows");
        assert(!rewritten.carried_col_idxs().intersects(dropped_var_set) &&
               "a dropped variable occurs in a rewritten clause");

        // Against an empty root trail no stored row is `0 = 0` and any two distinct
        // rows are independent, so the first two rows are a legal pair.
        rewritten.watched0 = 0;
        rewritten.watched1 = rewritten.rows() <= 1 ? 0 : 1;
        plan.rewritten_is_active.push_back(1);
        plan.rewritten_clauses.push_back(std::move(rewritten));
    }

    plan.history_entry.trail_rows_before.reserve(state.trail.span().rows());
    for (const Equation& row : state.trail.span()) {
        plan.history_entry.trail_rows_before.push_back(row);
    }
    return plan;
}

bool renumbering_frees_enough_words(std::size_t active_num_vars, std::size_t num_dropped_vars) {
    assert(num_dropped_vars <= active_num_vars && "more variables dropped than there are");
    const auto words = [](std::size_t num_vars) {
        return (num_vars + DenseGF2Vector::bits_per_word) / DenseGF2Vector::bits_per_word;
    };
    const std::size_t words_now = words(active_num_vars);
    const std::size_t freed_words = words_now - words(active_num_vars - num_dropped_vars);
    return freed_words >= 1 && 100 * freed_words >= min_freed_word_percent * words_now;
}

} // namespace xorcle
