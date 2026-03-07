#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "heuristics/DecisionProcedure.h"

namespace heuristics {
enum class ClauseSelectorName;
}

namespace stats {

/// @brief Solver result classification used in run statistics.
enum class SolveResult {
    Timeout, ///< Solver did not finish before the timeout.
    Unsat,   ///< Solver proved the instance unsatisfiable.
    Sat      ///< Solver found a satisfying assignment.
};

/// @brief Per-run solver statistics.
struct RunStats {
    SolveResult result = SolveResult::Timeout; ///< Final status of the run.
    double time_s = 0.0;                       ///< Wall-clock time in seconds.
    std::size_t num_vars = 0;                  ///< Number of variables in the instance.
    std::size_t start_clauses = 0;             ///< Number of clauses before solving.
    std::size_t final_clauses = 0;             ///< Number of clauses after solving.
    std::size_t num_conflicts = 0;             ///< Count of conflicts encountered.
    std::size_t num_propagations = 0;          ///< Count of unit propagations performed.
    std::size_t num_attempt_props = 0;         ///< Count of attempted propagations.
    std::size_t num_watch_updates = 0;         ///< Count of watch updates performed.
    std::size_t num_restarts = 0;              ///< Count of restarts performed.
    std::size_t decisions = 0;                 ///< Count of decisions made.
    std::size_t fallback_decisions = 0;        ///< Count of decisions that used fallback logic.
    std::optional<std::string> sat_assignment; ///< Satisfying assignment string, when available.
};

/// @brief Metadata describing an aggregation request.
struct AggregateMeta {
    std::string run_id;                               ///< Unique id for the aggregation run.
    std::string timestamp_utc;                        ///< Timestamp in UTC (ISO-like format).
    std::string file;                                 ///< Input file path.
    heuristics::DecisionProcedureName decision_procedure; ///< Decision procedure used.
    heuristics::DecisionPhase decision_phase = heuristics::DecisionPhase::unsat; ///< Decision phase used.
    heuristics::ClauseSelectorName clause_selector;   ///< Clause selector used.
    std::size_t threads = 0;                          ///< Number of threads requested.
    std::size_t timeout_s = 0;                        ///< Timeout per run in seconds.
    bool deterministic = true;                        ///< Whether the run is deterministic.
    std::size_t runs_requested = 1;                   ///< Number of runs requested.
    std::optional<double> cvsids_learned_base;        ///< CVsids learned-clause base multiplier, when applicable.
    std::optional<double> cvsids_decay;               ///< CVsids decay factor, when applicable.
    std::optional<double> cvsids_bump;                ///< CVsids bump amount, when applicable.
};

/// @brief Aggregated statistics across multiple runs.
struct AggregateStats {
    std::string run_id;                               ///< Unique id for the aggregation run.
    std::string timestamp_utc;                        ///< Timestamp in UTC (ISO-like format).
    std::string file;                                 ///< Input file path.
    heuristics::DecisionProcedureName decision_procedure; ///< Decision procedure used.
    heuristics::DecisionPhase decision_phase = heuristics::DecisionPhase::unsat; ///< Decision phase used.
    heuristics::ClauseSelectorName clause_selector;   ///< Clause selector used.
    std::size_t threads = 0;                          ///< Number of threads requested.
    std::size_t timeout_s = 0;                        ///< Timeout per run in seconds.
    bool deterministic = true;                        ///< Whether the run is deterministic.
    std::optional<double> cvsids_learned_base;        ///< CVsids learned-clause base multiplier, when applicable.
    std::optional<double> cvsids_decay;               ///< CVsids decay factor, when applicable.
    std::optional<double> cvsids_bump;                ///< CVsids bump amount, when applicable.

    std::size_t runs_requested = 1;                   ///< Number of runs requested.
    std::size_t runs_completed = 0;                   ///< Number of non-timeout runs completed.
    std::size_t timeout_count = 0;                    ///< Number of timed-out runs.
    std::size_t sat_count = 0;                        ///< Number of SAT runs.
    std::size_t unsat_count = 0;                      ///< Number of UNSAT runs.
    std::string result;                               ///< Selected run classification.

    std::size_t num_vars = 0;                         ///< Number of variables in the instance.
    std::size_t start_clauses = 0;                    ///< Number of clauses before solving.

    std::size_t final_clauses = 0;                    ///< Number of clauses after solving.
    std::size_t learned_clauses = 0;                  ///< Learned clause count (final - start).
    double time_s = 0.0;                              ///< Selected run time in seconds.
    std::size_t conflicts = 0;                        ///< Selected run conflicts.
    std::size_t propagations = 0;                     ///< Selected run propagations.
    std::size_t attempt_props = 0;                    ///< Selected run attempted propagations.
    std::size_t watch_updates = 0;                    ///< Selected run watch updates.
    std::size_t restarts = 0;                         ///< Selected run restarts.
    std::size_t decisions = 0;                        ///< Selected run decisions.
    std::size_t fallback_decisions = 0;               ///< Selected run fallback decisions.
    std::optional<double> fallback_pct;               ///< Fallback decisions / decisions * 100.
    std::optional<double> prop_per_conflict;          ///< Propagations per conflict.
    std::optional<double> attempts_per_prop_watch_conflict; ///< attempt_props / (propagations + watch_updates + conflicts).

    std::optional<std::string> sat_assignment;        ///< Satisfying assignment for selected run.
};

/// @brief Convert a decision procedure enum to a stable string label.
std::string to_string(heuristics::DecisionProcedureName name);

/// @brief Convert a decision phase enum to a stable string label.
std::string to_string(heuristics::DecisionPhase phase);

/// @brief Convert a clause selector enum to a stable string label.
std::string to_string(heuristics::ClauseSelectorName name);

/// @brief Return true if a decision procedure is inherently nondeterministic.
bool is_nondeterministic(heuristics::DecisionProcedureName name);

/// @brief Divide two doubles and return std::nullopt when denom is zero.
std::optional<double> safe_div(double numer, double denom);

/// @brief Aggregate multiple RunStats into a summary record.
AggregateStats aggregate_runs(const AggregateMeta& meta, const std::vector<RunStats>& runs);

} // namespace stats
