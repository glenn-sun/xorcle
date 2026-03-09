#include "util/Stats.h"

namespace stats {

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

std::string to_string(const SolveResult result) {
    switch (result) {
        case SolveResult::Sat:
            return "SAT";
        case SolveResult::Unsat:
            return "UNSAT";
    }
    return "unknown";
}

} // namespace stats
