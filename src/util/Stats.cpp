#include "util/Stats.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

#include "heuristics/DecisionProcedure.h"
#include "heuristics/ClauseSelector.h"

namespace stats {

std::string to_string(heuristics::DecisionProcedureName name) {
    using heuristics::DecisionProcedureName;
    switch (name) {
        case DecisionProcedureName::fixed_unit:
            return "fixed_unit";
        case DecisionProcedureName::random_clause_sum:
            return "random_clause_sum";
        case DecisionProcedureName::vsids_unit:
            return "vsids_unit";
        case DecisionProcedureName::vsids_clause:
            return "vsids_clause";
        case DecisionProcedureName::cvsids:
            return "cvsids";
        case DecisionProcedureName::berkmin:
            return "berkmin";
        case DecisionProcedureName::cbh:
            return "cbh";
    }
    return "unknown";
}

std::string to_string(heuristics::ClauseSelectorName name) {
    using heuristics::ClauseSelectorName;
    switch (name) {
        case ClauseSelectorName::cyclic:
            return "cyclic";
        case ClauseSelectorName::lt_watchlist:
            return "lt_watchlist";
        case ClauseSelectorName::exact_watchlist:
            return "exact_watchlist";
    }
    return "unknown";
}

bool is_nondeterministic(heuristics::DecisionProcedureName name) {
    using heuristics::DecisionProcedureName;
    return name == DecisionProcedureName::random_clause_sum ||
           name == DecisionProcedureName::vsids_clause ||
           name == DecisionProcedureName::cvsids ||
           name == DecisionProcedureName::berkmin ||
           name == DecisionProcedureName::cbh;
}

std::optional<double> safe_div(double numer, double denom) {
    if (denom == 0.0) {
        return std::nullopt;
    }
    return numer / denom;
}

AggregateStats aggregate_runs(const AggregateMeta& meta, const std::vector<RunStats>& runs) {
    AggregateStats out;
    out.run_id = meta.run_id;
    out.timestamp_utc = meta.timestamp_utc;
    out.file = meta.file;
    out.decision_procedure = meta.decision_procedure;
    out.clause_selector = meta.clause_selector;
    out.threads = meta.threads;
    out.timeout_s = meta.timeout_s;
    out.deterministic = meta.deterministic;
    out.runs_requested = meta.runs_requested;

    if (runs.empty()) {
        return out;
    }

    out.num_vars = runs.front().num_vars;
    out.start_clauses = runs.front().start_clauses;

    std::size_t completed = 0;
    std::size_t timeouts = 0;
    std::size_t sat_count = 0;
    std::size_t unsat_count = 0;

    for (const auto& run : runs) {
        if (run.result == SolveResult::Timeout) {
            ++timeouts;
            continue;
        }
        ++completed;
        if (run.result == SolveResult::Sat) {
            ++sat_count;
        } else if (run.result == SolveResult::Unsat) {
            ++unsat_count;
        }
    }

    std::vector<std::size_t> order(runs.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        bool a_timeout = runs[a].result == SolveResult::Timeout;
        bool b_timeout = runs[b].result == SolveResult::Timeout;
        if (a_timeout != b_timeout) {
            return b_timeout;
        }
        if (runs[a].time_s != runs[b].time_s) {
            return runs[a].time_s < runs[b].time_s;
        }
        return a < b;
    });

    std::size_t selected_index = order[order.size() / 2];
    const RunStats& selected = runs[selected_index];

    out.runs_completed = completed;
    out.timeout_count = timeouts;
    out.sat_count = sat_count;
    out.unsat_count = unsat_count;

    switch (selected.result) {
        case SolveResult::Timeout:
            out.result = "TIMEOUT";
            break;
        case SolveResult::Sat:
            out.result = "SAT";
            break;
        case SolveResult::Unsat:
            out.result = "UNSAT";
            break;
    }

    out.final_clauses = selected.final_clauses;
    out.learned_clauses = selected.final_clauses >= selected.start_clauses
                              ? selected.final_clauses - selected.start_clauses
                              : 0;
    out.time_s = selected.time_s;
    out.conflicts = selected.num_conflicts;
    out.propagations = selected.num_propagations;
    out.attempt_props = selected.num_attempt_props;
    out.watch_updates = selected.num_watch_updates;
    out.decisions = selected.decisions;
    out.fallback_decisions = selected.fallback_decisions;
    out.fallback_pct = safe_div(100.0 * static_cast<double>(selected.fallback_decisions),
                                static_cast<double>(selected.decisions));
    out.prop_per_conflict = safe_div(static_cast<double>(selected.num_propagations),
                                     static_cast<double>(selected.num_conflicts));
    out.attempts_per_prop = safe_div(static_cast<double>(selected.num_attempt_props),
                                     static_cast<double>(selected.num_propagations));
    out.attempts_per_watch = safe_div(static_cast<double>(selected.num_attempt_props),
                                      static_cast<double>(selected.num_watch_updates));

    if (selected.result == SolveResult::Sat && selected.sat_assignment.has_value()) {
        out.sat_assignment = selected.sat_assignment;
    }

    return out;
}

} // namespace stats
