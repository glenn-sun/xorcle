/// @file
/// @brief Helper functions for converting active-space solver data back into
/// original-space assignments and proof payloads.

#include "search/OriginalSpace.h"

#include <stdexcept>

namespace search {

algebra::Equation lift_with_variable_map(const algebra::Equation& active_eqn,
                                         const std::vector<std::size_t>& active_var_to_original,
                                         std::size_t original_num_vars) {
    if (active_eqn.size() != active_var_to_original.size() + 1) {
        throw std::runtime_error("lift_with_variable_map: width mismatch");
    }

    algebra::Equation original_eqn{original_num_vars};
    for (std::size_t active_var = 0; active_var < active_var_to_original.size(); ++active_var) {
        if (active_eqn.element(active_var)) {
            original_eqn.set(active_var_to_original[active_var]);
        }
    }
    if (active_eqn.element(active_var_to_original.size())) {
        original_eqn.set(original_num_vars);
    }
    return original_eqn;
}

bit::vector<> reconstruct_original_assignment(const bit::vector<>& active_assignment,
                                              const std::vector<CompactionHistoryEntry>& compaction_history,
                                              const std::vector<std::size_t>& active_var_to_original,
                                              std::size_t original_num_vars) {
    bit::vector<> assignment_active = active_assignment;

    for (auto history_it = compaction_history.rbegin(); history_it != compaction_history.rend(); ++history_it) {
        const CompactionHistoryEntry& history_entry = *history_it;
        if (assignment_active.size() != history_entry.transform.new_num_vars) {
            throw std::runtime_error("reconstruct_original_assignment: active width mismatch");
        }

        bit::vector<> assignment_before = bit::vector<>::zeros(history_entry.transform.old_num_vars);
        for (std::size_t active_var = 0; active_var < history_entry.transform.new_num_vars; ++active_var) {
            if (assignment_active.element(active_var)) {
                assignment_before.set(history_entry.transform.new_to_old[active_var]);
            }
        }

        for (auto row_it = history_entry.trail_rows_before.rbegin();
             row_it != history_entry.trail_rows_before.rend();
             ++row_it) {
            const algebra::Equation& trail_row_before = *row_it;
            const std::size_t pivot_var = trail_row_before.first_set();
            if (pivot_var == bit::vector<>::npos || pivot_var >= history_entry.transform.old_num_vars) {
                throw std::runtime_error(
                    "reconstruct_original_assignment: compaction history contains an invalid trail pivot");
            }

            bool pivot_value = trail_row_before.element(history_entry.transform.old_num_vars);
            for (std::size_t col = trail_row_before.next_set(pivot_var);
                 col != bit::vector<>::npos;
                 col = trail_row_before.next_set(col)) {
                if (col == history_entry.transform.old_num_vars) {
                    continue;
                }
                pivot_value ^= assignment_before.element(col);
            }

            if (pivot_value) {
                assignment_before.set(pivot_var);
            } else {
                assignment_before.reset(pivot_var);
            }
        }

        assignment_active = std::move(assignment_before);
    }

    if (assignment_active.size() == original_num_vars) {
        return assignment_active;
    }

    bit::vector<> original_assignment = bit::vector<>::zeros(original_num_vars);
    if (assignment_active.size() == active_var_to_original.size()) {
        for (std::size_t active_var = 0; active_var < active_var_to_original.size(); ++active_var) {
            if (assignment_active.element(active_var)) {
                original_assignment.set(active_var_to_original[active_var]);
            }
        }
        return original_assignment;
    }

    throw std::runtime_error("reconstruct_original_assignment: failed to recover original assignment width");
}

algebra::Clause build_proof_clause_in_original_space(
    const algebra::Clause& active_clause,
    const std::vector<ProofAssumption>& proof_assumptions,
    const std::vector<std::size_t>& active_var_to_original,
    std::size_t original_num_vars) {
    algebra::Clause proof_clause(original_num_vars);
    for (std::size_t row_index = 0; row_index < active_clause.rows(); ++row_index) {
        proof_clause.add_row(lift_with_variable_map(active_clause.row(row_index),
                                                    active_var_to_original,
                                                    original_num_vars));
    }

    for (const ProofAssumption& proof_assumption : proof_assumptions) {
        proof_clause.add_row(proof_assumption.original_eqn);
    }

    proof_clause.watched0 = 0;
    proof_clause.watched1 = (proof_clause.rows() > 1) ? 1 : 0;
    proof_clause.true_witness = 0;
    proof_clause.next_watch_search_start = 0;
    return proof_clause;
}

RootUnsatProofPlan build_root_unsat_proof_plan(
    const std::vector<std::optional<heuristics::ClauseId>>& trail_reasons,
    const std::vector<ProofAssumption>& proof_assumptions,
    std::size_t original_num_vars) {
    RootUnsatProofPlan plan;
    plan.conflict_reason_ids.reserve(trail_reasons.size());
    for (const auto& reason_clause_id : trail_reasons) {
        if (reason_clause_id.has_value()) {
            plan.conflict_reason_ids.push_back(*reason_clause_id);
        }
    }

    if (proof_assumptions.empty()) {
        return plan;
    }

    algebra::Clause guard_clause(original_num_vars);
    for (const ProofAssumption& proof_assumption : proof_assumptions) {
        guard_clause.add_row(proof_assumption.original_eqn);
        if (proof_assumption.reason_clause_id.has_value()) {
            plan.final_reason_ids.push_back(*proof_assumption.reason_clause_id);
        }
    }
    guard_clause.watched0 = 0;
    guard_clause.watched1 = (guard_clause.rows() > 1) ? 1 : 0;
    guard_clause.true_witness = 0;
    guard_clause.next_watch_search_start = 0;
    plan.guard_clause = std::move(guard_clause);
    return plan;
}

} // namespace search
