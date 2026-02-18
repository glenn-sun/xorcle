#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "MaxHeap.h"
#include "heuristics/ClauseSelector.h"
#include "heuristics/DecisionProcedure.h"
#include "heuristics/Types.h"

namespace heuristics {

/// @brief CVsids activity parameters for clauses.
struct CVsidsParams {
    MaxHeap<ClauseId> activity_heap; ///< Heap keyed by clause activity.

    double inc_amt = 1;  ///< Activity increment.
    double decay = 0.95; ///< Activity decay factor.

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
    ClauseId num_clauses = 0;   ///< Number of clauses tracked in the heap.

    std::size_t num_vars;       ///< Number of variables in the problem.

    /// @brief Construct state for a given number of variables.
    explicit CVsidsState(std::size_t n);

    /// @brief Ensure unit equations are initialized.
    void ensure_unit_eqns();

    /// @brief Reset activity heap to a clean state.
    void reset_activity();
};

/// @brief Decision procedure using CVsids clause activity.
class CVsidsDecision : public DecisionProcedure {
protected:
    std::shared_ptr<CVsidsState> state; ///< Shared CVsids state.

public:
    /// @brief Construct with optional shared state.
    explicit CVsidsDecision(std::size_t n, std::shared_ptr<CVsidsState> shared_state = nullptr);

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
};

/// @brief Clause selector and decision procedure combined using CVsids.
class CVsids : public DecisionProcedure, public ClauseSelector {
protected:
    std::shared_ptr<CVsidsState> state; ///< Shared CVsids state.
    ClauseId next_id = 0;               ///< Next clause id in cyclic order.
    std::size_t count = 0;              ///< Number of clauses visited in current pass.
    std::optional<ClauseId> pending_restart_clause; ///< Optional restart clause id.

    /// @brief Reset cycle position when a new equation is learned.
    void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) override;

public:
    /// @brief Construct a combined CVsids decision/selection strategy.
    explicit CVsids(std::size_t n);

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

    /// @brief Reset cycle position when a decision is made.
    void update_with_decision(const Equation& reduced_eqn, const Trail& trail) override;

    /// @brief Return a hint for the last decision clause.
    std::optional<ClauseId> decision_clause_hint() const override;

    /// @brief Return the next clause id in cyclic order.
    std::optional<ClauseId> next() override;

    /// @brief Add a starting clause and reset cycle counters.
    void add_starting_clause(const Clause& clause) override;

    /// @brief Add an asserting clause after backjumping.
    void add_asserting_clause(const Clause& clause, const Trail& trail, std::size_t jumpable_levels) override;

    /// @brief Apply a pending restart clause if present.
    void update_with_decision_clause(std::optional<ClauseId> clause_id) override;
};
} // namespace heuristics
