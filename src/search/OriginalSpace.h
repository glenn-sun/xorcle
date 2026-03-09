/// @file
/// @brief Internal helpers for converting active-space solver data back into
/// the original input variable indexing.

#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "algebra/RowSpace.h"
#include "algebra/Equation.h"
#include "bit/vector.h"
#include "heuristics/Types.h"
#include "search/Compaction.h"

namespace search {

/// @brief Lift an equation from a current active space into original variable indexing.
/// @param active_eqn Equation in the active space described by `active_var_to_original`.
/// @param active_var_to_original Active variable index -> original variable index map.
/// @param original_num_vars Number of variables in the original input space.
/// @return Lifted equation in original variable indexing.
algebra::Equation lift_with_variable_map(const algebra::Equation& active_eqn,
                                         const std::vector<std::size_t>& active_var_to_original,
                                         std::size_t original_num_vars);

/// @brief Reconstruct the original-width assignment from the current active assignment.
bit::vector<> reconstruct_original_assignment(const bit::vector<>& active_assignment,
                                              const std::vector<CompactionHistoryEntry>& compaction_history,
                                              const std::vector<std::size_t>& active_var_to_original,
                                              std::size_t original_num_vars);

/// @brief Build a learned proof clause in original-space indexing.
algebra::Clause build_proof_clause_in_original_space(
    const algebra::Clause& active_clause,
    const std::vector<ProofAssumption>& proof_assumptions,
    const std::vector<std::size_t>& active_var_to_original,
    std::size_t original_num_vars);

/// @brief Payload needed to emit the final UNSAT proof once the root trail is contradictory.
struct RootUnsatProofPlan {
    std::vector<heuristics::ClauseId> conflict_reason_ids;
    std::optional<algebra::Clause> guard_clause;
    std::vector<heuristics::ClauseId> final_reason_ids;
};

/// @brief Build the root-UNSAT proof payload from trail reasons and carried proof assumptions.
RootUnsatProofPlan build_root_unsat_proof_plan(
    const std::vector<std::optional<heuristics::ClauseId>>& trail_reasons,
    const std::vector<ProofAssumption>& proof_assumptions,
    std::size_t original_num_vars);

} // namespace search
