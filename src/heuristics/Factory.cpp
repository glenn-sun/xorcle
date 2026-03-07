#include "heuristics/Factory.h"

#include "heuristics/ExactWatchlist.h"

namespace heuristics {
std::shared_ptr<DecisionProcedure> create_decision_procedure(DecisionProcedureName name,
                                                             std::size_t num_vars,
                                                             DecisionPhase phase,
                                                             const CVsidsConfig& cvsids_config) {
    std::shared_ptr<DecisionProcedure> proc;
    switch (name) {
        case DecisionProcedureName::fixed_unit:
            proc = std::make_shared<FixedUnit>(num_vars);
            break;
        case DecisionProcedureName::random_clause_sum:
            proc = std::make_shared<RandomClauseSum>(num_vars);
            break;
        case DecisionProcedureName::vsids_unit:
            proc = std::make_shared<VsidsUnit>(num_vars);
            break;
        case DecisionProcedureName::vsids_clause:
            proc = std::make_shared<VsidsClause>(num_vars);
            break;
        case DecisionProcedureName::cvsids:
            proc = std::make_shared<CVsidsDecision>(num_vars, cvsids_config);
            break;
    }
    if (proc) {
        proc->set_decision_phase(phase);
    }
    return proc;
}

std::shared_ptr<ClauseSelector> create_clause_selector(ClauseSelectorName name, std::size_t num_vars) {
    switch (name) {
        case ClauseSelectorName::cyclic:
            return std::make_shared<Cyclic>(num_vars);
        case ClauseSelectorName::lt_watchlist:
            return std::make_shared<LTWatchlist>(num_vars);
        case ClauseSelectorName::exact_watchlist:
            return std::make_shared<ExactWatchlist>(num_vars);
    }
    return nullptr;
}
} // namespace heuristics
