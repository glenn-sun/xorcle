#include "heuristics/Factory.h"

#include "heuristics/ExactWatchlist.h"

namespace heuristics {
std::shared_ptr<DecisionProcedure> create_decision_procedure(DecisionProcedureName name, std::size_t num_vars) {
    switch (name) {
        case DecisionProcedureName::fixed_unit:
            return std::make_shared<FixedUnit>(num_vars);
        case DecisionProcedureName::random_unit:
            return std::make_shared<RandomUnit>(num_vars);
        case DecisionProcedureName::random_eq:
            return std::make_shared<RandomEq>(num_vars);
        case DecisionProcedureName::random_clause_sum:
            return std::make_shared<RandomClauseSum>(num_vars);
        case DecisionProcedureName::vsids_unit:
            return std::make_shared<VsidsUnit>(num_vars);
        case DecisionProcedureName::vsids_multiple:
            return std::make_shared<VsidsMultiple>(num_vars);
        case DecisionProcedureName::vsids_clause:
            return std::make_shared<VsidsClause>(num_vars);
        case DecisionProcedureName::cvsids:
            return std::make_shared<CVsidsDecision>(num_vars);
        case DecisionProcedureName::cvsids_sum:
            return std::make_shared<CVsidsSumDecision>(num_vars);
    }
    return nullptr;
}

std::shared_ptr<ClauseSelector> create_clause_selector(ClauseSelectorName name, std::size_t num_vars) {
    switch (name) {
        case ClauseSelectorName::cyclic:
            return std::make_shared<Cyclic>(num_vars);
        case ClauseSelectorName::random_cyclic:
            return std::make_shared<RandomCyclic>(num_vars);
        case ClauseSelectorName::lt_watchlist:
            return std::make_shared<LTWatchlist>(num_vars);
        case ClauseSelectorName::exact_watchlist:
            return std::make_shared<ExactWatchlist>(num_vars);
        case ClauseSelectorName::cvsids:
        case ClauseSelectorName::cvsids_sum:
            return std::make_shared<CVsidsSelector>(num_vars);
    }
    return nullptr;
}
} // namespace heuristics
