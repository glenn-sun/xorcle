#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "util/index_set.h"

namespace xorcle {

enum class SolveResult {
    unsat,
    sat
};

/// @brief Per-run solver statistics and configuration metadata.
struct RunStats {
    std::string run_id;
    std::string timestamp_utc;
    std::string file;
    std::string decision_procedure;
    std::string decision_phase;
    std::string clause_selector;
    std::uint64_t seed = 0;                 ///< Reproduces the run exactly.
    /// @brief The CSIDS activity parameters, present only when CSIDS is in use.
    std::optional<double> csids_initial_activity;
    std::optional<double> csids_decay;
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
    std::size_t num_reused_levels = 0; ///< Decision levels restarts kept instead of discarding.
    std::size_t decisions = 0;
    std::size_t fallback_decisions = 0;

    /// @brief Whether clause minimization ran; its counters are reported only
    /// when it did.
    bool minimize = false;
    std::size_t num_minimized_clauses = 0;
    std::size_t minimize_dim_before = 0;
    std::size_t minimize_dim_after = 0;
    /// @brief The satisfying assignment, indexed by variable in the original
    /// numbering; set only on SAT.
    std::optional<IndexSet> sat_assignment;
};

/// @brief `SAT`/`UNSAT`, the CSV spelling.
///
/// `ConsoleStatsWriter` writes `SATISFIABLE`/`UNSATISFIABLE` and does not go
/// through this.
std::string to_string(SolveResult result);

/// @brief Number of clauses learned during the run, floored at zero.
std::size_t learned_clause_count(const RunStats& stats);

} // namespace xorcle
