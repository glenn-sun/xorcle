#pragma once

/// @file stats.h
/// @brief Per-run solver statistics.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "util/bitmap.h"

namespace xorcle {

/// @brief Solver result classification used in run statistics.
enum class SolveResult {
    unsat, ///< Solver proved the instance unsatisfiable.
    sat    ///< Solver found a satisfying assignment.
};

/// @brief Per-run solver statistics and configuration metadata.
///
/// The three configuration labels are plain strings rather than the heuristic
/// enums the 0.1 tree used. `util/` sits below `heuristics/`, so it cannot name
/// `DecisionProcedureName`, `DecisionPhase` or `ClauseSelectorName` without
/// inverting the dependency; the CLI, which owns the enum-to-name tables
/// already, fills these in. The console and CSV sinks print them verbatim, so
/// the output format is unchanged.
struct RunStats {
    std::string run_id;                     ///< Unique id for the CLI run.
    std::string timestamp_utc;              ///< Run start timestamp in UTC.
    std::string file;                       ///< Input file path.
    std::string decision_procedure;         ///< Decision procedure label.
    std::string decision_phase;             ///< Decision phase label.
    std::string clause_selector;            ///< Clause selector label.
    std::uint64_t seed = 0;                 ///< Seed required to reproduce the run exactly.
    std::optional<double> csids_initial_activity; ///< CSIDS A0, when CSIDS is in use.
    std::optional<double> csids_decay;      ///< CSIDS alpha, when CSIDS is in use.
    std::optional<double> csids_bump;       ///< CSIDS delta, when CSIDS is in use.

    SolveResult result = SolveResult::unsat; ///< Final solver result.
    double time_s = 0.0;                     ///< Wall-clock time in seconds.
    std::size_t num_vars = 0;                ///< Number of variables in the instance.
    std::size_t start_clauses = 0;           ///< Number of clauses before solving.
    std::size_t final_clauses = 0;           ///< Number of clauses after solving.
    std::size_t num_conflicts = 0;           ///< Count of conflicts encountered.
    std::size_t num_propagations = 0;        ///< Count of unit propagations performed.
    std::size_t num_attempt_props = 0;       ///< Count of attempted propagations.
    std::size_t num_watch_updates = 0;       ///< Count of watch updates performed.
    std::size_t num_restarts = 0;            ///< Count of restarts performed.
    std::size_t decisions = 0;               ///< Count of decisions made.
    std::size_t fallback_decisions = 0;      ///< Count of decisions that used fallback logic.
    std::optional<Bitmap> sat_assignment;    ///< Original-space satisfying assignment, when available.
};

/// @brief Convert a solve result enum to a stable string label.
/// @note Returns `SAT`/`UNSAT`, the CSV spelling. The console sink writes
/// `SATISFIABLE`/`UNSATISFIABLE` and does not go through this.
std::string to_string(SolveResult result);

/// @brief Number of clauses learned during the run, floored at zero.
std::size_t learned_clause_count(const RunStats& stats);

} // namespace xorcle
