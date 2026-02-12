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

    ClauseId next_id = 0;
    std::size_t count = 0;
    std::optional<ClauseId> last_decision_clause;
    bool restart_from_decision = false;

    std::size_t num_vars;
    const std::vector<Clause>* clauses = nullptr;

    explicit CVsidsState(std::size_t n);

    void bind_clauses(const std::vector<Clause>* clauses_ptr);
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
    void update_with_resolved_clause(ClauseId id) override;
};

class CVsidsSumDecision : public CVsidsDecision {
public:
    using CVsidsDecision::CVsidsDecision;
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;
};

class CVsidsSelector : public ClauseSelector {
protected:
    std::shared_ptr<CVsidsState> state;

    void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) override;

public:
    explicit CVsidsSelector(std::size_t n, std::shared_ptr<CVsidsState> shared_state = nullptr);

    std::optional<ClauseId> next() override;
    void add_starting_clause(const Clause& clause) override;
    void add_asserting_clause(const Clause& clause, const Trail& trail, std::size_t jumpable_levels) override;
    void update_with_decision(const Equation& reduced_eqn, const Trail& trail) override;
};

class CVsids : public DecisionProcedure, public ClauseSelector {
protected:
    std::shared_ptr<CVsidsState> state;

    void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) override;

public:
    explicit CVsids(std::size_t n);

    void reinitialize() override;
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;
    void update_with_asserting_clause(const Clause& clause) override;
    void update_with_resolved_clause(ClauseId id) override;
    void update_with_decision(const Equation& reduced_eqn, const Trail& trail) override;

    std::optional<ClauseId> next() override;
    void add_starting_clause(const Clause& clause) override;
    void add_asserting_clause(const Clause& clause, const Trail& trail, std::size_t jumpable_levels) override;
};

class CVsidsSum : public CVsids {
public:
    using CVsids::CVsids;
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;
};

} // namespace heuristics
