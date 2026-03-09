#include "heuristics/Factory.h"

#include <stdexcept>

#include "heuristics/ExactWatchlist.h"

namespace heuristics {
std::shared_ptr<DecisionProcedure> create_decision_procedure(DecisionProcedureName name,
                                                             std::size_t num_vars,
                                                             DecisionPhase phase,
                                                             util::Seed seed,
                                                             const CVsidsConfig& cvsids_config) {
    std::shared_ptr<DecisionProcedure> proc;
    switch (name) {
        case DecisionProcedureName::fixed_literals:
            proc = std::make_shared<FixedLiterals>(num_vars, seed);
            break;
        case DecisionProcedureName::variable_vsids:
            proc = std::make_shared<VariableVsids>(num_vars, seed);
            break;
        case DecisionProcedureName::clause_vsids:
            proc = std::make_shared<ClauseVsids>(num_vars, seed, cvsids_config);
            break;
    }
    if (proc) {
        proc->set_decision_phase(phase);
        return proc;
    }
    throw std::logic_error("create_decision_procedure: unsupported decision procedure enum");
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
    throw std::logic_error("create_clause_selector: unsupported clause selector enum");
}
} // namespace heuristics
