#pragma once

#include <queue>
#include <variant>
#include "Equation.h"
#include "RowSpace.h"
#include "MaxHeap.h"

enum class DecisionProcedureName { fixed_unit, random_unit, random_eq, vsids_unit };

class DecisionProcedure {
protected:
    std::size_t num_vars;
public:
    DecisionProcedure(std::size_t n) : num_vars(n) {}
    virtual ~DecisionProcedure() = default;
    virtual void reinitialize() = 0;
    virtual Equation make_decision(const RowSpace& trail_span) = 0;
    virtual void update_with_critical_equation(const Equation& eqn) {};
    virtual void update_with_asserting_clause(const Clause& clause) {};
};

class FixedUnit : public DecisionProcedure {
private:
    std::vector<Equation> decision_basis;
public:
    FixedUnit(std::size_t n) : DecisionProcedure(n) { reinitialize(); }
    void reinitialize() override;
    Equation make_decision(const RowSpace& trail_span) override;
};

class RandomUnit : public DecisionProcedure {
private:
    std::vector<Equation> decision_basis;
public:
    RandomUnit(std::size_t n) : DecisionProcedure(n) { reinitialize(); }
    void reinitialize() override;
    Equation make_decision(const RowSpace& trail_span) override;
};

class RandomEq : public DecisionProcedure {
private:
    std::vector<Equation> decision_basis;
public:
    RandomEq(std::size_t n) : DecisionProcedure(n) { reinitialize(); }
    void reinitialize() override;
    Equation make_decision(const RowSpace& trail_span) override;
};

struct VsidsParams {
    MaxHeap<Equation> activity_heap;

    double inc_amt = 1;
    double decay = 0.95;

    VsidsParams() : activity_heap() { }

    void bump_activity(const Equation& eqn);
};

class VsidsUnit : public DecisionProcedure {
private:
    VsidsParams vsids_params;
public:
    VsidsUnit(std::size_t n) : DecisionProcedure(n) { reinitialize(); }
    void reinitialize() override ;
    Equation make_decision(const RowSpace& trail_span) override;
    void update_with_critical_equation(const Equation& eqn) override;
    void update_with_asserting_clause(const Clause& clause) override;
};