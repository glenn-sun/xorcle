/// @file
/// @brief Helper functions for solver compaction: decide whether compaction applies,
/// rewrite the active problem into a smaller variable space, and record the data
/// needed to lift assignments and proofs back to original-space indexing later.

#include "search/Compaction.h"

#include <optional>
#include <stdexcept>
#include <string>

#include "search/OriginalSpace.h"
#include "util/DecisionUtils.h"

namespace search {
namespace {

std::optional<std::size_t> find_row_index(const algebra::Clause& clause,
                                          const algebra::Equation& clause_row) {
    for (std::size_t row_index = 0; row_index < clause.rows(); ++row_index) {
        if (clause.row(row_index) == clause_row) {
            return row_index;
        }
    }
    return std::nullopt;
}

std::size_t first_distinct_row(std::size_t num_rows, std::size_t row_to_avoid) {
    for (std::size_t row_index = 0; row_index < num_rows; ++row_index) {
        if (row_index != row_to_avoid) {
            return row_index;
        }
    }
    return 0;
}

void rebuild_clause_watch_metadata_after_compaction(algebra::Clause& rewritten_clause,
                                                    const algebra::Clause& old_clause,
                                                    const algebra::RowSpace& trail_span,
                                                    const CompactionTransform& transform) {
    if (rewritten_clause.rows() <= 1) {
        rewritten_clause.watched0 = 0;
        rewritten_clause.watched1 = 0;
        rewritten_clause.true_witness = 0;
        rewritten_clause.next_watch_search_start = 0;
        return;
    }

    auto project_old_row = [&](std::size_t row_index, const char* label) -> algebra::Equation {
        if (row_index >= old_clause.rows()) {
            throw std::logic_error(
                std::string("rebuild_clause_watch_metadata_after_compaction: invalid ") + label + " index");
        }
        algebra::Equation reduced_row = old_clause.row(row_index);
        trail_span.reduce(reduced_row);
        return transform.project_equation(reduced_row);
    };

    const std::optional<std::size_t> mapped_watched0 =
        find_row_index(rewritten_clause, project_old_row(old_clause.watched0, "watched0"));
    const std::optional<std::size_t> mapped_watched1 =
        find_row_index(rewritten_clause, project_old_row(old_clause.watched1, "watched1"));
    const std::optional<std::size_t> mapped_true_witness =
        find_row_index(rewritten_clause, project_old_row(old_clause.true_witness, "true_witness"));

    std::size_t watched0 = mapped_watched0.value_or(mapped_watched1.value_or(0));
    if (watched0 >= rewritten_clause.rows()) {
        watched0 = 0;
    }

    std::size_t watched1 = mapped_watched1.value_or(first_distinct_row(rewritten_clause.rows(), watched0));
    if (watched1 >= rewritten_clause.rows() || watched1 == watched0) {
        watched1 = first_distinct_row(rewritten_clause.rows(), watched0);
    }

    rewritten_clause.watched0 = watched0;
    rewritten_clause.watched1 = watched1;
    rewritten_clause.true_witness = mapped_true_witness.value_or(watched0);
    if (rewritten_clause.true_witness >= rewritten_clause.rows()) {
        rewritten_clause.true_witness = watched0;
    }
    rewritten_clause.next_watch_search_start = old_clause.next_watch_search_start % rewritten_clause.rows();
}

std::vector<ProofAssumption> collect_proof_assumptions_from_trail(
    const CompactionInputs& inputs,
    const CompactionHistoryEntry& history_entry) {
    std::vector<ProofAssumption> new_assumptions;
    new_assumptions.reserve(inputs.trail.reasons().size());

    auto already_recorded = [&](const algebra::Equation& original_eqn) {
        for (const ProofAssumption& existing_assumption : inputs.existing_proof_assumptions) {
            if (existing_assumption.original_eqn == original_eqn) {
                return true;
            }
        }
        for (const ProofAssumption& new_assumption : new_assumptions) {
            if (new_assumption.original_eqn == original_eqn) {
                return true;
            }
        }
        return false;
    };

    for (std::size_t learned_index = 0; learned_index < inputs.trail.reasons().size(); ++learned_index) {
        const std::size_t trail_row_index = inputs.trail.order_map()[learned_index];
        const algebra::Equation& active_eqn = inputs.trail.span().row(trail_row_index);
        algebra::Equation original_eqn = lift_with_variable_map(
            active_eqn, history_entry.active_var_to_original_before, inputs.original_num_vars);
        if (already_recorded(original_eqn)) {
            continue;
        }
        new_assumptions.push_back(ProofAssumption{original_eqn, inputs.trail.reasons()[learned_index]});
    }

    return new_assumptions;
}

} // namespace

bool should_compact_before_decision(const CompactionInputs& inputs) {
    if (inputs.trail.size() != 1) {
        return false;
    }
    if (inputs.trail.span().rows() < 1) {
        return false;
    }
    if (inputs.has_compacted_once) {
        const std::size_t conflicts_since_last_compaction =
            inputs.num_conflicts - inputs.conflicts_at_last_compaction;
        if (conflicts_since_last_compaction < inputs.compaction_conflict_interval) {
            return false;
        }
    }
    if (inputs.active_num_vars == 0) {
        return false;
    }

    const bit::vector<>& pivot_cols = inputs.trail.span().get_pivot_cols();
    if (pivot_cols.size() != inputs.active_num_vars + 1) {
        throw std::logic_error("should_compact_before_decision: trail pivot width mismatch");
    }
    if (pivot_cols.element(inputs.active_num_vars)) {
        throw std::logic_error("should_compact_before_decision: constant column cannot be a compaction pivot");
    }

    std::size_t pivot_count = 0;
    pivot_cols.if_set_call([&](std::size_t col) {
        if (col < inputs.active_num_vars) {
            ++pivot_count;
        }
    });
    return pivot_count != 0;
}

CompactionTransform build_compaction_transform(const Trail& trail, std::size_t active_num_vars) {
    const bit::vector<>& pivot_cols = trail.span().get_pivot_cols();
    if (pivot_cols.size() != active_num_vars + 1) {
        throw std::logic_error("build_compaction_transform: trail pivot width mismatch");
    }
    if (pivot_cols.element(active_num_vars)) {
        throw std::logic_error("build_compaction_transform: constant column cannot be a compaction pivot");
    }
    return CompactionTransform::from_pivots(active_num_vars, pivot_cols);
}

CompactionPlan build_compaction_plan(const CompactionInputs& inputs,
                                     const CompactionTransform& transform) {
    CompactionPlan compaction_plan;
    compaction_plan.transform = transform;

    const std::size_t num_clauses = inputs.clauses.size();
    compaction_plan.rewritten_clauses.reserve(num_clauses);
    compaction_plan.clause_is_active = inputs.clause_is_active;
    if (compaction_plan.clause_is_active.size() != num_clauses) {
        throw std::logic_error("build_compaction_plan: clause active flags are desynchronized");
    }

    util::ClauseSatisfactionWorkspace clause_satisfaction_workspace(inputs.active_num_vars);
    const algebra::Equation false_clause_row =
        algebra::Equation::unit(transform.new_num_vars + 1, transform.new_num_vars);

    for (heuristics::ClauseId clause_id = 0; clause_id < num_clauses; ++clause_id) {
        const algebra::Clause& active_clause = inputs.clauses[clause_id];
        const bool clause_was_active = inputs.clause_is_active[clause_id] != 0;

        bool clause_becomes_inactive = !clause_was_active;
        if (clause_was_active) {
            clause_becomes_inactive =
                util::clause_satisfied_by_trail(inputs.trail.span(), active_clause, clause_satisfaction_workspace);
        }

        algebra::Clause rewritten_clause(transform.new_num_vars);
        if (clause_becomes_inactive) {
            rewritten_clause.add_row(false_clause_row);
            rewritten_clause.watched0 = 0;
            rewritten_clause.watched1 = 0;
            rewritten_clause.true_witness = 0;
            rewritten_clause.next_watch_search_start = 0;
            compaction_plan.clause_is_active[clause_id] = 0;
            compaction_plan.rewritten_clauses.push_back(std::move(rewritten_clause));
            continue;
        }

        for (std::size_t row_index = 0; row_index < active_clause.rows(); ++row_index) {
            algebra::Equation reduced_row = active_clause.row(row_index);
            inputs.trail.span().reduce(reduced_row);
            rewritten_clause.add_row(transform.project_equation(reduced_row));
        }

        if (rewritten_clause.rows() == 0) {
            throw std::logic_error("build_compaction_plan: active clause projected to zero rows");
        }

        rebuild_clause_watch_metadata_after_compaction(
            rewritten_clause, active_clause, inputs.trail.span(), transform);
        compaction_plan.clause_is_active[clause_id] = 1;
        compaction_plan.rewritten_clauses.push_back(std::move(rewritten_clause));
    }

    if (compaction_plan.rewritten_clauses.size() != num_clauses) {
        throw std::logic_error("build_compaction_plan: rewritten clause count mismatch");
    }

    compaction_plan.history_entry.transform = transform;
    compaction_plan.history_entry.active_var_to_original_before = inputs.active_var_to_original;
    compaction_plan.history_entry.trail_rows_before.reserve(inputs.trail.span().rows());

    for (std::size_t row_index = 0; row_index < inputs.trail.span().rows(); ++row_index) {
        const algebra::Equation& active_eqn = inputs.trail.span().row(row_index);
        compaction_plan.history_entry.trail_rows_before.push_back(active_eqn);
    }

    compaction_plan.proof_assumptions =
        collect_proof_assumptions_from_trail(inputs, compaction_plan.history_entry);
    return compaction_plan;
}

} // namespace search
