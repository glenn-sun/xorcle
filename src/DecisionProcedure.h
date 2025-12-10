#pragma once

#include <queue>
#include <variant>
#include "Equation.h"
#include "RowSpace.h"

enum class DecisionProcedureName { fixed_unit, random_unit, random_eq, vsids_unit };

class DecisionProcedureBase {
protected:
    std::size_t num_vars;
public:
    DecisionProcedureBase(std::size_t n) : num_vars(n) {}
    virtual void reinitialize() = 0;
    virtual Equation make_decision(const RowSpace& trail_span) = 0;
    virtual void update_with_critical_equation(const Equation& eqn) {};
    virtual void update_with_asserting_clause(const Clause& clause) {};
};

class FixedUnit : public DecisionProcedureBase {
private:
    std::vector<Equation> decision_basis;
public:
    FixedUnit(std::size_t n) : DecisionProcedureBase(n) { reinitialize(); }
    void reinitialize() override;
    Equation make_decision(const RowSpace& trail_span) override;
};

class RandomUnit : public DecisionProcedureBase {
private:
    std::vector<Equation> decision_basis;
public:
    RandomUnit(std::size_t n) : DecisionProcedureBase(n) { reinitialize(); }
    void reinitialize() override;
    Equation make_decision(const RowSpace& trail_span) override;
};

class RandomEq : public DecisionProcedureBase {
private:
    std::vector<Equation> decision_basis;
public:
    RandomEq(std::size_t n) : DecisionProcedureBase(n) { reinitialize(); }
    void reinitialize() override;
    Equation make_decision(const RowSpace& trail_span) override;
};

struct VsidsComparator {
    const std::vector<double>* activity;
    VsidsComparator(const std::vector<double>& activity) : activity(&activity) {}
    bool operator()(std::size_t x, std::size_t y) const {
        return (*activity)[x] < (*activity)[y];
    }
};

struct VsidsParams {
    std::vector<double> activity;
    std::priority_queue<std::size_t, std::vector<std::size_t>, VsidsComparator> order_heap;
    double inc_amt = 1;
    double decay = 0.95;

    VsidsParams(std::size_t n) : activity(n, 0), order_heap(VsidsComparator(activity)) {};

    void bump_activity(std::size_t v);
};

class VsidsUnit : public DecisionProcedureBase {
private:
    VsidsParams vsids_params;
public:
    VsidsUnit(std::size_t n) : DecisionProcedureBase(n), vsids_params(n) { reinitialize(); }
    void reinitialize() override ;
    Equation make_decision(const RowSpace& trail_span) override;
    void update_with_critical_equation(const Equation& eqn) override;
    void update_with_asserting_clause(const Clause& clause) override;
};

using DecisionProcedure = std::variant<FixedUnit, RandomUnit, RandomEq, VsidsUnit>;