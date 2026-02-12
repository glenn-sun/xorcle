#pragma once

#include <memory>

#include "heuristics/ClauseSelector.h"
#include "heuristics/DecisionProcedure.h"
#include "heuristics/CVsids.h"

namespace heuristics {
std::shared_ptr<DecisionProcedure> create_decision_procedure(DecisionProcedureName name, std::size_t num_vars);
std::shared_ptr<ClauseSelector> create_clause_selector(ClauseSelectorName name, std::size_t num_vars);
} // namespace heuristics
