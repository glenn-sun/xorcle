#pragma once

#include <vector>
#include <optional>
#include <cassert>
#include "DecisionProcedure.h"
#include "ClauseSelector.h"
#include "RowSpace.h"
#include "Equation.h"
#include "Trail.h"
#include "MaxHeap.h"

struct CVsidsParams {
    MaxHeap<ClauseId> activity_heap;

    double inc_amt = 1;
    double decay = 0.95;

    CVsidsParams() : activity_heap() {}

    void bump_activity(ClauseId id);
};

class CVsids : public DecisionProcedure, public ClauseSelector {
protected:
    CVsidsParams params;
    Equation eqn_false;

    ClauseId next_id = 0;
    std::size_t count = 0;
    std::optional<ClauseId> last_decision_clause;
    bool restart_from_decision = false;

    bool clause_satisfied(const RowSpace& trail_span, const Clause& clause) const;
    Equation decide_from_units(const RowSpace& trail_span) const;

    void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) override;

public:
    CVsids(std::size_t n);

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
