#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "heuristics/DecisionProcedure.h"
#include "heuristics/ClauseSelector.h"
#include "heuristics/Types.h"
#include "MaxHeap.h"

namespace heuristics {

struct CVsidsParams {
    MaxHeap<ClauseId> activity_heap;

    double inc_amt = 1;
    double decay = 0.95;

    CVsidsParams() : activity_heap() {}

    void bump_activity(ClauseId id);
};

struct CVsidsState {
    CVsidsParams params;
    Equation eqn_false;
    std::vector<Equation> unit_eqns;

    std::optional<ClauseId> last_decision_clause;
    ClauseId num_clauses = 0;

    std::size_t num_vars;

    explicit CVsidsState(std::size_t n);

    void ensure_unit_eqns();
    void reset_activity();
};

class CVsidsDecision : public DecisionProcedure {
protected:
    std::shared_ptr<CVsidsState> state;

public:
    explicit CVsidsDecision(std::size_t n, std::shared_ptr<CVsidsState> shared_state = nullptr);

    void reinitialize() override;
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;
    void update_with_asserting_clause(const Clause& clause) override;
    void update_with_new_clause(const Clause& clause, ClauseId id) override;
    void update_with_resolved_clause(ClauseId id) override;
    std::optional<ClauseId> decision_clause_hint() const override;
};

class CVsids : public DecisionProcedure, public ClauseSelector {
protected:
    std::shared_ptr<CVsidsState> state;
    ClauseId next_id = 0;
    std::size_t count = 0;
    std::optional<ClauseId> pending_restart_clause;

    void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) override;

public:
    explicit CVsids(std::size_t n);

    void reinitialize() override;
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;
    void update_with_asserting_clause(const Clause& clause) override;
    void update_with_new_clause(const Clause& clause, ClauseId id) override;
    void update_with_resolved_clause(ClauseId id) override;
    void update_with_decision(const Equation& reduced_eqn, const Trail& trail) override;
    std::optional<ClauseId> decision_clause_hint() const override;

    std::optional<ClauseId> next() override;
    void add_starting_clause(const Clause& clause) override;
    void add_asserting_clause(const Clause& clause, const Trail& trail, std::size_t jumpable_levels) override;
    void update_with_decision_clause(std::optional<ClauseId> clause_id) override;
};
} // namespace heuristics
