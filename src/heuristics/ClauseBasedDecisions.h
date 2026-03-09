#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "MaxHeap.h"
#include "heuristics/DecisionProcedure.h"
#include "heuristics/Types.h"
#include "util/RandomUtils.h"

namespace heuristics {

/// @brief Clause-activity parameters for Clause VSIDS.
struct CVsidsParams {
    MaxHeap<ClauseId> activity_heap; ///< Heap keyed by clause activity.
    double inc_amt = 1.0;            ///< Current activity increment used for bumps.
    double learned_base = 1.0;       ///< Learned-clause initial activity multiplier on current increment.
    double decay = 0.95;             ///< Activity decay factor.

    /// @brief Increase activity for a clause id and rescale if needed.
    void bump_activity(ClauseId id);
};

/// @brief Shared Clause VSIDS state used by the decision procedure.
/// @note `num_clauses`, `activity_heap`, `last_decision_clause`, and
/// `pending_learned_priority` must describe the same clause-id domain.
struct CVsidsState {
    CVsidsParams params;                   ///< Activity parameters and heap.
    std::vector<Equation> unit_eqns;       ///< Unit equations used by literal fallback.

    std::optional<ClauseId> last_decision_clause;     ///< Last clause used for a decision.
    std::optional<double> pending_learned_priority;   ///< Initial priority for the next learned clause.
    ClauseId num_clauses = 0;                          ///< Number of tracked clause ids.
    double initial_inc_amt = 1.0;                     ///< Configured initial increment.
    std::size_t num_vars;                             ///< Number of active variables.

    explicit CVsidsState(std::size_t n, const CVsidsConfig& config = {});

    /// @brief Ensure unit equations are initialized for the current active width.
    void ensure_unit_eqns();

    /// @brief Reset clause activity for a fresh run.
    void reset_activity();
};

/// @brief Decision procedure using Clause VSIDS clause activity.
class ClauseVsids : public DecisionProcedure {
protected:
    /// @brief Shared Clause VSIDS state.
    std::shared_ptr<CVsidsState> state;

    /// @brief Reused clause-satisfaction scratch state.
    util::ClauseSatisfactionWorkspace clause_sat_ws;

public:
    explicit ClauseVsids(std::size_t n,
                         util::Seed seed,
                         const CVsidsConfig& config = {},
                         std::shared_ptr<CVsidsState> shared_state = nullptr);

    /// @brief Reset activity, RNG state, and counters for a fresh run.
    void reinitialize() override;

    /// @brief Choose a decision from the highest-priority unsatisfied clause.
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;

    /// @brief Decay activity after learning an asserting clause.
    void update_with_asserting_clause(const Clause& clause) override;

    /// @brief Register a new clause in the shared activity heap.
    void update_with_new_clause(const Clause& clause, ClauseId id) override;

    /// @brief Bump activity for a resolved clause.
    void update_with_resolved_clause(ClauseId id) override;

    /// @brief Return a hint for the last decision clause.
    std::optional<ClauseId> decision_clause_hint() const override;

    void update_with_compaction(const search::CompactionTransform& transform,
                                const RowSpace& trail_span,
                                const std::vector<Clause>& clauses,
                                const std::vector<std::uint8_t>& clause_active) override;

private:
    /// @brief RNG for sampling equations from decision clauses.
    util::Rng clause_equation_rng;
};

} // namespace heuristics
