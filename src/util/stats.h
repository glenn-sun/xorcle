#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "util/column_set.h"

namespace xorcle {

enum class SolveResult {
    unsat,
    sat
};

/// @brief Per-run solver statistics and configuration metadata.
///
/// The three configuration labels are plain strings rather than the heuristic
/// enums: `util/` sits below `heuristics/`, so it cannot name
/// `DecisionProcedureName`, `DecisionPhase` or `ClauseSelectorName` without
/// inverting the dependency. The CLI, which owns the enum-to-name tables, fills
/// them in and the sinks print them verbatim.
struct RunStats {
    std::string run_id;
    std::string timestamp_utc;
    std::string file;
    std::string decision_procedure;
    std::string decision_phase;
    std::string clause_selector;
    std::uint64_t seed = 0;                 ///< Reproduces the run exactly.
    std::optional<double> csids_initial_activity; ///< CSIDS A0, alpha and delta,
    std::optional<double> csids_decay;      ///< present only when CSIDS is in use.
    std::optional<double> csids_bump;

    SolveResult result = SolveResult::unsat;
    double time_s = 0.0;                     ///< Wall-clock.
    std::size_t num_vars = 0;
    std::size_t start_clauses = 0;
    std::size_t final_clauses = 0;
    std::size_t num_conflicts = 0;
    std::size_t num_propagations = 0;
    std::size_t num_attempt_props = 0;
    std::size_t num_watch_updates = 0;
    std::size_t num_restarts = 0;
    std::size_t decisions = 0;
    std::size_t fallback_decisions = 0;      ///< Decisions that used fallback logic.
    std::optional<ColumnSet> sat_assignment;    ///< Original-space, when available.
};

/// @brief `SAT`/`UNSAT`, the CSV spelling. The console sink writes
/// `SATISFIABLE`/`UNSATISFIABLE` and does not go through this.
std::string to_string(SolveResult result);

/// @brief Number of clauses learned during the run, floored at zero.
std::size_t learned_clause_count(const RunStats& stats);

} // namespace xorcle
