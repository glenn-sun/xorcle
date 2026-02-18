#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace heuristics {
enum class DecisionProcedureName;
enum class ClauseSelectorName;
}

namespace stats {

enum class SolveResult { Timeout, Unsat, Sat };

struct RunStats {
    SolveResult result = SolveResult::Timeout;
    double time_s = 0.0;
    std::size_t num_vars = 0;
    std::size_t start_clauses = 0;
    std::size_t final_clauses = 0;
    std::size_t num_conflicts = 0;
    std::size_t num_propagations = 0;
    std::size_t num_attempt_props = 0;
    std::size_t num_watch_updates = 0;
    std::size_t decisions = 0;
    std::size_t fallback_decisions = 0;
    std::optional<std::string> sat_assignment;
};

struct AggregateMeta {
    std::string run_id;
    std::string timestamp_utc;
    std::string file;
    heuristics::DecisionProcedureName decision_procedure;
    heuristics::ClauseSelectorName clause_selector;
    std::size_t threads = 0;
    std::size_t timeout_s = 0;
    bool deterministic = true;
    std::size_t runs_requested = 1;
};

struct AggregateStats {
    std::string run_id;
    std::string timestamp_utc;
    std::string file;
    heuristics::DecisionProcedureName decision_procedure;
    heuristics::ClauseSelectorName clause_selector;
    std::size_t threads = 0;
    std::size_t timeout_s = 0;
    bool deterministic = true;

    std::size_t runs_requested = 1;
    std::size_t runs_completed = 0;
    std::size_t timeout_count = 0;
    std::size_t sat_count = 0;
    std::size_t unsat_count = 0;
    std::string result;

    std::size_t num_vars = 0;
    std::size_t start_clauses = 0;

    std::size_t final_clauses = 0;
    std::size_t learned_clauses = 0;
    double time_s = 0.0;
    std::size_t conflicts = 0;
    std::size_t propagations = 0;
    std::size_t attempt_props = 0;
    std::size_t watch_updates = 0;
    std::size_t decisions = 0;
    std::size_t fallback_decisions = 0;
    std::optional<double> fallback_pct;
    std::optional<double> prop_per_conflict;
    std::optional<double> attempts_per_prop;
    std::optional<double> attempts_per_watch;

    std::optional<std::string> sat_assignment;
};

std::string to_string(heuristics::DecisionProcedureName name);
std::string to_string(heuristics::ClauseSelectorName name);
bool is_nondeterministic(heuristics::DecisionProcedureName name);
std::optional<double> safe_div(double numer, double denom);

AggregateStats aggregate_runs(const AggregateMeta& meta, const std::vector<RunStats>& runs);

} // namespace stats
