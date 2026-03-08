#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "MaxHeap.h"
#include "heuristics/DecisionProcedure.h"
#include "heuristics/Types.h"

namespace heuristics {

/// @brief CVsids activity parameters for clauses.
struct CVsidsParams {
    MaxHeap<ClauseId> activity_heap; ///< Heap keyed by clause activity.

    double inc_amt = 1.0;       ///< Current activity increment used for bumps.
    double learned_base = 1.0;  ///< Learned-clause initial activity multiplier on current increment.
    double decay = 0.95;        ///< Activity decay factor.

    /// @brief Construct with an empty activity heap.
    CVsidsParams() : activity_heap() {}

    /// @brief Increase activity for a clause id and rescale if needed.
    void bump_activity(ClauseId id);
};

/// @brief Shared CVsids state used across decision and selection.
struct CVsidsState {
    CVsidsParams params;        ///< Activity parameters and heap.
    Equation eqn_false;         ///< Cached false equation (1 = 0).
    std::vector<Equation> unit_eqns; ///< Unit equations for fallback decisions.

    std::optional<ClauseId> last_decision_clause; ///< Last clause used to decide.
    std::optional<double> pending_learned_priority; ///< Initial priority for the next learned asserting clause.
    ClauseId num_clauses = 0;   ///< Number of clauses tracked in the heap.
    double initial_inc_amt = 1.0; ///< Configured initial increment (CLI bump parameter).

    std::size_t num_vars;       ///< Number of variables in the problem.

    /// @brief Construct state for a given number of variables.
    explicit CVsidsState(std::size_t n, const CVsidsConfig& config = {});

    /// @brief Ensure unit equations are initialized.
    void ensure_unit_eqns();

    /// @brief Reset activity heap to a clean state.
    void reset_activity();
};

/// @brief Decision procedure using CVsids clause activity.
class CVsidsDecision : public DecisionProcedure {
protected:
    std::shared_ptr<CVsidsState> state; ///< Shared CVsids state.
    util::ClauseSatisfactionWorkspace clause_sat_ws; ///< Reused clause-satisfaction scratch state.

public:
    /// @brief Construct with optional shared state.
    explicit CVsidsDecision(std::size_t n,
                            const CVsidsConfig& config = {},
                            std::shared_ptr<CVsidsState> shared_state = nullptr);

    /// @brief Reset activity and counters.
    void reinitialize() override;

    /// @brief Choose a decision based on clause activity.
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;

    /// @brief Decay activity after learning an asserting clause.
    void update_with_asserting_clause(const Clause& clause) override;

    /// @brief Register a new clause in the activity heap.
    void update_with_new_clause(const Clause& clause, ClauseId id) override;

    /// @brief Bump activity for a resolved clause.
    void update_with_resolved_clause(ClauseId id) override;

    /// @brief Return a hint for the last decision clause.
    std::optional<ClauseId> decision_clause_hint() const override;

    void update_with_compaction(const search::CompactionTransform& transform,
                                const RowSpace& trail_span,
                                const std::vector<Clause>& clauses,
                                const std::vector<std::uint8_t>& clause_active) override;
};

} // namespace heuristics
