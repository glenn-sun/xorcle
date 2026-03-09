#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "bit/vector.h"
#include "heuristics/ClauseSelector.h"
#include "heuristics/DecisionProcedure.h"

namespace stats {

/// @brief Solver result classification used in run statistics.
enum class SolveResult {
    Unsat, ///< Solver proved the instance unsatisfiable.
    Sat    ///< Solver found a satisfying assignment.
};

/// @brief Per-run solver statistics and configuration metadata.
struct RunStats {
    std::string run_id;                               ///< Unique id for the CLI run.
    std::string timestamp_utc;                        ///< Run start timestamp in UTC.
    std::string file;                                 ///< Input file path.
    heuristics::DecisionProcedureName decision_procedure = heuristics::DecisionProcedureName::clause_vsids; ///< Decision procedure used.
    heuristics::DecisionPhase decision_phase = heuristics::DecisionPhase::unsat; ///< Decision phase used.
    heuristics::ClauseSelectorName clause_selector = heuristics::ClauseSelectorName::cyclic; ///< Clause selector used.
    std::uint64_t seed = 0;                           ///< Seed required to reproduce the run exactly.
    std::optional<double> cvsids_learned_base;        ///< Clause VSIDS learned-clause base multiplier.
    std::optional<double> cvsids_decay;               ///< Clause VSIDS decay factor.
    std::optional<double> cvsids_bump;                ///< Clause VSIDS bump amount.

    SolveResult result = SolveResult::Unsat;          ///< Final solver result.
    double time_s = 0.0;                              ///< Wall-clock time in seconds.
    std::size_t num_vars = 0;                         ///< Number of variables in the instance.
    std::size_t start_clauses = 0;                    ///< Number of clauses before solving.
    std::size_t final_clauses = 0;                    ///< Number of clauses after solving.
    std::size_t num_conflicts = 0;                    ///< Count of conflicts encountered.
    std::size_t num_propagations = 0;                 ///< Count of unit propagations performed.
    std::size_t num_attempt_props = 0;                ///< Count of attempted propagations.
    std::size_t num_watch_updates = 0;                ///< Count of watch updates performed.
    std::size_t num_restarts = 0;                     ///< Count of restarts performed.
    std::size_t decisions = 0;                        ///< Count of decisions made.
    std::size_t fallback_decisions = 0;               ///< Count of decisions that used fallback logic.
    std::optional<bit::vector<>> sat_assignment;      ///< Original-space satisfying assignment, when available.
};

/// @brief Convert a decision procedure enum to a stable string label.
std::string to_string(heuristics::DecisionProcedureName name);

/// @brief Convert a decision phase enum to a stable string label.
std::string to_string(heuristics::DecisionPhase phase);

/// @brief Convert a clause selector enum to a stable string label.
std::string to_string(heuristics::ClauseSelectorName name);

/// @brief Convert a solve result enum to a stable string label.
std::string to_string(SolveResult result);

} // namespace stats
