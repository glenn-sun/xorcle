#pragma once

#include <optional>
#include <utility>

#include "algebra/Equation.h"
#include "heuristics/ClauseSelector.h"
#include "search/Trail.h"

namespace search {
class PropagationEngine {
private:
    std::size_t num_vars;
    algebra::Equation working_eqn;
    algebra::Equation working_eqn2;

    std::optional<algebra::Equation> unit_prop_one_clause(heuristics::ClauseSelector& clause_sel,
                                                         const Trail& trail,
                                                         const algebra::Equation& eqn_false,
                                                         heuristics::ClauseId id,
                                                         std::size_t& num_watch_updates);

public:
    explicit PropagationEngine(std::size_t num_vars);

    std::optional<std::pair<algebra::Equation, heuristics::ClauseId>> unit_propagate_once(
        heuristics::ClauseSelector& clause_sel,
        const Trail& trail,
        const algebra::Equation& eqn_false,
        std::size_t& num_attempt_props,
        std::size_t& num_watch_updates);
};
} // namespace search
