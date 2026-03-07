#pragma once

#include <cstddef>
#include <memory>

#include "heuristics/ClauseBasedDecisions.h"
#include "heuristics/ClauseSelector.h"
#include "heuristics/DecisionProcedure.h"

namespace heuristics {

/// @brief Create a decision procedure instance by name.
std::shared_ptr<DecisionProcedure> create_decision_procedure(DecisionProcedureName name,
                                                             std::size_t num_vars,
                                                             DecisionPhase phase,
                                                             const CVsidsConfig& cvsids_config = {});

/// @brief Create a clause selector instance by name.
std::shared_ptr<ClauseSelector> create_clause_selector(ClauseSelectorName name, std::size_t num_vars);

} // namespace heuristics
