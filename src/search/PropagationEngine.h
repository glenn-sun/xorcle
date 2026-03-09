#pragma once

#include <cstddef>
#include <optional>
#include <utility>

#include "algebra/Equation.h"
#include "heuristics/ClauseSelector.h"
#include "search/Trail.h"

namespace search {

/// @brief Unit propagation engine operating on clauses and a trail.
class PropagationEngine {
private:
    std::size_t num_vars; ///< Number of variables in the problem.
    algebra::Equation working_eqn; ///< Scratch equation for reductions.
    algebra::Equation working_eqn2; ///< Secondary scratch equation.

    /// @brief Attempt to propagate from a single clause.
    /// @param clause_sel Clause selector providing clause storage and watch updates.
    /// @param trail Current trail state.
    /// @param id Clause id to attempt.
    /// @param zero_watch_hint Optional zero-trigger returned by the selector.
    /// @param num_watch_updates Incremented when a watch is updated.
    /// @return A propagated equation or conflict equation if implied; nullopt otherwise.
    /// @pre `id < clause_sel.get_num_clauses()`.
    /// @pre If the clause is unit then `zero_watch_hint` is empty.
    /// @pre If the clause is non-unit and `zero_watch_hint` is present, it names
    /// a reduced zero trigger that currently permits propagation or watch motion.
    /// @note `WatchType::Sum` hints are normalized to updating `watched1`.
    std::optional<algebra::Equation> unit_prop_one_clause(heuristics::ClauseSelector& clause_sel,
                                                         const Trail& trail,
                                                         heuristics::ClauseId id,
                                                         std::optional<heuristics::WatchType> zero_watch_hint,
                                                         std::size_t& num_watch_updates);

public:
    /// @brief Construct a propagation engine for a given variable count.
    explicit PropagationEngine(std::size_t num_vars);

    /// @brief Reconfigure internal scratch storage for a new active variable count.
    void reconfigure(std::size_t num_vars);

    /// @brief Attempt one unit propagation step.
    /// @param clause_sel Clause selector providing clause storage and watch updates.
    /// @param trail Current trail state.
    /// @param num_attempt_props Incremented for each attempted clause.
    /// @param num_watch_updates Incremented when a watch is updated.
    /// @return The propagated equation and reason clause id if propagation occurs.
    /// @throws std::logic_error if the selector returns an invalid or inactive clause id.
    std::optional<std::pair<algebra::Equation, heuristics::ClauseId>> unit_propagate_once(
        heuristics::ClauseSelector& clause_sel,
        const Trail& trail,
        std::size_t& num_attempt_props,
        std::size_t& num_watch_updates);

};
} // namespace search
