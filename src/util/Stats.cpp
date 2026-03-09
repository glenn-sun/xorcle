#include "util/Stats.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

#include "heuristics/DecisionProcedure.h"
#include "heuristics/ClauseSelector.h"

namespace stats {

namespace {

std::optional<double> divide_or_nullopt(double numer, double denom) {
    if (denom == 0.0) {
        return std::nullopt;
    }
    return numer / denom;
}

void validate_run_shape(const std::vector<RunStats>& runs) {
    if (runs.empty()) {
        return;
    }

    const std::size_t expected_num_vars = runs.front().num_vars;
    const std::size_t expected_start_clauses = runs.front().start_clauses;
    for (const RunStats& run : runs) {
        if (run.num_vars != expected_num_vars) {
            throw std::invalid_argument("aggregate_runs: run set mixes different variable counts");
        }
        if (run.start_clauses != expected_start_clauses) {
            throw std::invalid_argument("aggregate_runs: run set mixes different starting clause counts");
        }
    }
}

} // namespace

std::string to_string(heuristics::DecisionProcedureName name) {
    using heuristics::DecisionProcedureName;
    switch (name) {
        case DecisionProcedureName::fixed_literals:
            return "fixed_literals";
        case DecisionProcedureName::variable_vsids:
            return "variable_vsids";
        case DecisionProcedureName::clause_vsids:
            return "clause_vsids";
    }
    return "unknown";
}

std::string to_string(heuristics::DecisionPhase phase) {
    using heuristics::DecisionPhase;
    switch (phase) {
        case DecisionPhase::unsat:
            return "unsat";
        case DecisionPhase::sat:
            return "sat";
        case DecisionPhase::random:
            return "random";
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

AggregateStats aggregate_runs(const AggregateMeta& meta, const std::vector<RunStats>& runs) {
    AggregateStats out;
    out.run_id = meta.run_id;
    out.timestamp_utc = meta.timestamp_utc;
    out.file = meta.file;
    out.decision_procedure = meta.decision_procedure;
    out.decision_phase = meta.decision_phase;
    out.clause_selector = meta.clause_selector;
    out.seed = meta.seed;
    out.cvsids_learned_base = meta.cvsids_learned_base;
    out.cvsids_decay = meta.cvsids_decay;
    out.cvsids_bump = meta.cvsids_bump;

    if (runs.empty()) {
        return out;
    }

    validate_run_shape(runs);
    out.num_vars = runs.front().num_vars;
    out.start_clauses = runs.front().start_clauses;

    std::size_t sat_count = 0;
    std::size_t unsat_count = 0;

    for (const auto& run : runs) {
        if (run.result == SolveResult::Timeout) {
            continue;
        }
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
    out.restarts = selected.num_restarts;
    out.decisions = selected.decisions;
    out.fallback_decisions = selected.fallback_decisions;
    out.fallback_pct = divide_or_nullopt(100.0 * static_cast<double>(selected.fallback_decisions),
                                         static_cast<double>(selected.decisions));
    out.prop_per_conflict = divide_or_nullopt(static_cast<double>(selected.num_propagations),
                                              static_cast<double>(selected.num_conflicts));
    out.attempts_per_prop_watch_conflict =
        divide_or_nullopt(static_cast<double>(selected.num_attempt_props),
                          static_cast<double>(selected.num_propagations + selected.num_watch_updates +
                                              selected.num_conflicts));

    if (selected.result == SolveResult::Sat && selected.sat_assignment.has_value()) {
        out.sat_assignment = selected.sat_assignment;
    }

    return out;
}

} // namespace stats
