#pragma once

#include <deque>
#include <list>
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

enum class DecisionProcedureName { fixed_unit, random_clause_sum, vsids_unit, vsids_clause, cvsids, berkmin, cbh };

class DecisionProcedure {
protected:
    std::size_t num_vars;
    std::size_t decisions = 0;
    std::size_t fallback_decisions = 0;

    void record_decision(bool used_fallback) {
        ++decisions;
        if (used_fallback) {
            ++fallback_decisions;
        }
    }

    void reset_stats() {
        decisions = 0;
        fallback_decisions = 0;
    }

public:
    DecisionProcedure(std::size_t n) : num_vars(n) {}
    virtual ~DecisionProcedure() = default;
    virtual void reinitialize() = 0;
    virtual Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) = 0;
    virtual void update_with_critical_equation(const Equation& eqn) {};
    virtual void update_with_asserting_clause(const Clause& clause) {};
    virtual void update_with_new_clause(const Clause& clause, ClauseId id) {};
    virtual void update_with_resolved_clause(ClauseId id) {};
    virtual std::optional<ClauseId> decision_clause_hint() const { return std::nullopt; }

    struct DecisionStats {
        std::size_t decisions = 0;
        std::size_t fallback_decisions = 0;
    };

    virtual DecisionStats decision_stats() const {
        return DecisionStats{decisions, fallback_decisions};
    }
};

class FixedUnit : public DecisionProcedure {
private:
    std::vector<Equation> decision_basis;
public:
    FixedUnit(std::size_t n) : DecisionProcedure(n) { reinitialize(); }
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
                        record_decision(false);
                        return *maybe_eqn;
                    };
                }
            }
        }
    }
};

class BerkminDecision : public DecisionProcedure {
private:
    std::deque<ClauseId> conflict_clauses;
    bool pending_conflict_clause = false;
    std::vector<Equation> unit_eqns;
    std::optional<ClauseId> last_decision_clause;

public:
    BerkminDecision(std::size_t n) : DecisionProcedure(n) { reinitialize(); }
    void reinitialize() override;
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;
    void update_with_asserting_clause(const Clause& clause) override;
    void update_with_new_clause(const Clause& clause, ClauseId id) override;
    std::optional<ClauseId> decision_clause_hint() const override;
};

class CBHDecision : public DecisionProcedure {
private:
    std::list<ClauseId> clause_list;
    std::vector<std::list<ClauseId>::iterator> positions;
    bool list_built = false;
    std::list<ClauseId>::iterator current;
    std::vector<ClauseId> pending_responsible;
    std::vector<char> pending_mark;
    bool pending_conflict_clause = false;
    std::optional<ClauseId> pending_conflict_clause_id;
    std::vector<Equation> unit_eqns;
    std::optional<ClauseId> last_decision_clause;

    void ensure_unit_eqns();
    void build_initial_list(const std::vector<Clause>& clauses);
    void apply_pending_reorder(ClauseId conflict_id);

public:
    CBHDecision(std::size_t n) : DecisionProcedure(n) { reinitialize(); }
    void reinitialize() override;
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;
    void update_with_asserting_clause(const Clause& clause) override;
    void update_with_resolved_clause(ClauseId id) override;
    void update_with_new_clause(const Clause& clause, ClauseId id) override;
    std::optional<ClauseId> decision_clause_hint() const override;
};
} // namespace heuristics
