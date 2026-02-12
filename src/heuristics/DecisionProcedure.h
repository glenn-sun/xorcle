#pragma once

#include <queue>
#include <variant>
#include <vector>
#include <random>
#include <numeric>
#include <cassert>
#include <optional>
#include "algebra/Equation.h"
#include "algebra/RowSpace.h"
#include "MaxHeap.h"
#include "heuristics/Types.h"
#include "util/RandomUtils.h"

namespace heuristics {
using Clause = algebra::Clause;
using Equation = algebra::Equation;
using RowSpace = algebra::RowSpace;
using REFMatrix = algebra::REFMatrix;

enum class DecisionProcedureName { fixed_unit, random_unit, random_eq, vsids_unit, vsids_multiple, vsids_clause, cvsids, cvsids_sum, random_clause_sum };

class DecisionProcedure {
protected:
    std::size_t num_vars;
public:
    DecisionProcedure(std::size_t n) : num_vars(n) {}
    virtual ~DecisionProcedure() = default;
    virtual void reinitialize() = 0;
    virtual Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) = 0;
    virtual void update_with_critical_equation(const Equation& eqn) {};
    virtual void update_with_asserting_clause(const Clause& clause) {};
    virtual void update_with_starting_clause(const Clause& clause) {};
    virtual void update_with_resolved_clause(ClauseId id) {};
};

class FixedUnit : public DecisionProcedure {
private:
    std::vector<Equation> decision_basis;
public:
    FixedUnit(std::size_t n) : DecisionProcedure(n) { reinitialize(); }
    void reinitialize() override;
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>&) override;
};

class RandomUnit : public DecisionProcedure {
private:
    std::vector<Equation> decision_basis;
public:
    RandomUnit(std::size_t n) : DecisionProcedure(n) { reinitialize(); }
    void reinitialize() override;
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>&) override;
};

class RandomEq : public DecisionProcedure {
private:
    std::vector<Equation> decision_basis;
public:
    RandomEq(std::size_t n) : DecisionProcedure(n) { reinitialize(); }
    void reinitialize() override;
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>&) override;
};

class RandomClauseSum : public DecisionProcedure {
private:
    std::vector<Equation> decision_basis;
    std::vector<ClauseId> clause_perm;
    std::mt19937 rng;
public:
    RandomClauseSum(std::size_t n) : DecisionProcedure(n), rng(std::random_device{}()) { reinitialize(); }
    void reinitialize() override;
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;
};

struct VsidsParams {
    MaxHeap<Equation> activity_heap;

    double inc_amt = 1;
    double decay = 0.95;

    VsidsParams() : activity_heap() { }

    void bump_activity(const Equation& eqn);
};

class VsidsUnit : public DecisionProcedure {
protected:
    VsidsParams vsids_params;
    Equation eqn_false;
    std::vector<Equation> unit_eqns;

public:
    VsidsUnit(std::size_t n) : DecisionProcedure(n) { reinitialize(); eqn_false = Equation::unit(num_vars + 1, num_vars); }
    void reinitialize() override ;
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>&) override;
    void update_with_critical_equation(const Equation& eqn) override;
    void update_with_asserting_clause(const Clause& clause) override;
};

class VsidsMultiple : public VsidsUnit {
private:
    std::vector<int> weights;

public:
    VsidsMultiple(std::size_t n) : VsidsUnit(n), weights(n, 1) { weights[0] = 0; if (n > 2) weights[1] = n - 2; }
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>&) override {
        while (true) {
            int num_to_peek = util::weighted_random_index(weights) - 1;
            std::optional<Equation> maybe_eqn = vsids_params.activity_heap.extract_max();
            if (!maybe_eqn.has_value()) {
                reinitialize();
                maybe_eqn = vsids_params.activity_heap.extract_max();
            }
            auto next_eqns = vsids_params.activity_heap.peek_top_k(num_to_peek);
            for (auto& eqn : next_eqns) {
                *maybe_eqn += eqn;
            }
            trail_span.reduce(*maybe_eqn);
            if ((*maybe_eqn).any() && *maybe_eqn != eqn_false) {
                return *maybe_eqn;
            };
        }
    }
};

class VsidsClause : public VsidsUnit {
public:
    VsidsClause(std::size_t n) : VsidsUnit(n) { }
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override {
        while (true) {
            std::optional<Equation> maybe_eqn = vsids_params.activity_heap.extract_max();
            if (!maybe_eqn.has_value()) {
                reinitialize();
                maybe_eqn = vsids_params.activity_heap.extract_max();
            }
            
            std::size_t k = (*maybe_eqn).first_set();

            std::size_t start = util::random_int(clauses.size());
            for (std::size_t i = 0; i < clauses.size(); ++i) {
                std::size_t j = (start + i) % clauses.size();
                if (clauses[j].get_pivot_cols()[k]) {
                    assert((*maybe_eqn).size() == clauses[j].row(clauses[j].get_col_to_row(k)).size());
                    (*maybe_eqn).unsafe_copy(clauses[j].row(clauses[j].get_col_to_row(k)));
                    trail_span.reduce(*maybe_eqn);
                    if ((*maybe_eqn).any() && *maybe_eqn != eqn_false) {
                        return *maybe_eqn;
                    };
                }
            }
        }
    }
};
} // namespace heuristics
