#pragma once

#include <variant>
#include <queue>
#include <unordered_set>
#include <bit>
#include <algorithm>
#include <optional>
#include <vector>
#include "bit/vector.h"
#include "algebra/RowSpace.h"
#include "algebra/Equation.h"
#include "search/Trail.h"
#include "heuristics/Types.h"
#include <random>
#include <cassert>

namespace heuristics {
using Clause = algebra::Clause;
using Equation = algebra::Equation;
using Trail = search::Trail;

enum class WatchType { Zero, One, Sum };
enum class ClauseSelectorName { cyclic, random_cyclic, lt_watchlist, exact_watchlist, cvsids, cvsids_sum };

class ClauseSelector {
protected:
    std::vector<Clause> clauses;
    // Trail after learning, new eqn must be reduced by trail before learning
    virtual void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) {}
    virtual void add_clause(const Clause& clause) { clauses.push_back(clause); }
public:
    ClauseSelector() : clauses() {}
    virtual ~ClauseSelector() = default;

    std::size_t get_num_clauses() const { return clauses.size(); }
    const std::vector<Clause>& get_clauses() const { return clauses; }
    Clause& get_clause(ClauseId id) { return clauses[id]; }

    virtual std::optional<ClauseId> next() = 0;
    virtual void add_starting_clause(const Clause& clause) { add_clause(clause); }
    // Trail after backjumping
    virtual void add_asserting_clause(const Clause& clause, const Trail& trail, std::size_t jumpable_levels) { add_clause(clause); }
    virtual void update_with_propagation(const Equation& reduced_eqn, const Trail& trail) { update_with_new_equation(reduced_eqn, trail); };
    virtual void update_with_decision(const Equation& reduced_eqn, const Trail& trail) { update_with_new_equation(reduced_eqn, trail); };
    virtual void update_with_new_watch(ClauseId id, EquationId old_watch, WatchType wt, const Trail& trail) {};
};

class Cyclic : public ClauseSelector {
private:
    ClauseId id;
    std::size_t count;
public:
    Cyclic(std::size_t num_vars) : ClauseSelector(), id(0), count(0) {}
    std::optional<ClauseId> next() override {
        if (count == clauses.size()) {
            count = 0;
            return std::nullopt;
        }
        std::size_t old_id = id;
        id = (id + 1) % clauses.size();
        ++count;
        return old_id;
    }

    void add_clause(const Clause& clause) override {
        clauses.push_back(clause);
        id = clauses.size() - 1;
        count = 0;
    }

    void update_with_new_equation(const Equation& eqn, const Trail& trail) override { 
        count = 0;
    }
};

class RandomCyclic : public ClauseSelector {
private:
    std::mt19937 rng;
    std::size_t count;
    std::size_t index;
    std::vector<ClauseId> permutation;
public:
    RandomCyclic(std::size_t num_vars) : ClauseSelector(), rng(std::random_device{}()), count(0), index(0), permutation() {}
    std::optional<ClauseId> next() override {
        if (count == clauses.size()) {
            count = 0;
            return std::nullopt;
        }
        std::size_t old_index = index;
        index = (index + 1) % clauses.size();
        ++count;
        return permutation[old_index];
    }

    void add_clause(const Clause& clause) override {
        clauses.push_back(clause);
        std::shuffle(permutation.begin(), permutation.end(), rng);
        permutation.push_back(clauses.size() - 1);
        index = clauses.size() - 1;
        count = 0;
    }

    void update_with_new_equation(const Equation& eqn, const Trail& trail) override { 
        count = 0;
    }
};


class LTWatchlist : public ClauseSelector {
private:
    // Map from leading term to list of clauses watching 
    // some equation with that leading term
    std::vector<std::vector<ClauseId>> by_leading_term;

    std::vector<ClauseId> stack;
    std::vector<bool> stack_set;

    std::vector<ClauseId> unit_clauses;

    void push(ClauseId id) {
        if (!stack_set[id]) {
            stack.push_back(id);
            stack_set[id] = true;
        }
    }

    void append(const std::vector<ClauseId>& ids) {
        for (ClauseId id : ids) {
            push(id);
        }
    }

    ClauseId get_pop() {
        ClauseId id = stack.back();
        stack.pop_back();
        stack_set[id] = false;
        return id;
    }

public:
    LTWatchlist(std::size_t num_vars) : ClauseSelector(), by_leading_term(num_vars+1) {}
    
    std::optional<ClauseId> next() override {
        if (stack.empty()) {
            return std::nullopt;
        }
        return get_pop();
    }

    void add_clause(const Clause& clause) override {
        if (clause.rows() > 1) {
            by_leading_term[clause.row(clause.watched0).first_set()].push_back(clauses.size());
            by_leading_term[clause.row(clause.watched1).first_set()].push_back(clauses.size());
            stack_set.push_back(false);
        } else {
            unit_clauses.push_back(clauses.size());
            stack_set.push_back(true);
            stack.push_back(clauses.size());
        }
        clauses.push_back(clause);
    }

    void add_asserting_clause(const Clause& clause, const Trail& /*unused*/, std::size_t /*unused*/) override {
        add_clause(clause);
        
        stack.clear();
        std::fill(stack_set.begin(), stack_set.end(), false);
        append(unit_clauses);
        push(clauses.size() - 1);
    }

    void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) override {
        // Because of linear combinations, need to check all lists corresponding
        // to reducible leading terms in position at most reduced.first_set()

        for (std::size_t i = 0; i < reduced_eqn.first_set(); ++i) {
            if (trail.span().get_pivot_cols()[i]) {
                append(by_leading_term[i]);
            }
        }
        append(by_leading_term[reduced_eqn.first_set()]);
    }

    void update_with_new_watch(ClauseId id, EquationId old_watch, WatchType wt, const Trail& /*unused*/) override {
        auto& v = by_leading_term[clauses[id].row(old_watch).first_set()];
        std::erase(v, id);
        EquationId new_watch = 0;
        switch (wt) {
            case WatchType::Zero:
                new_watch = clauses[id].watched0;
                break;
            case WatchType::One:
                new_watch = clauses[id].watched1;
                break;
            case WatchType::Sum:
                // Should never happen
                assert(false && "WatchType::Sum not expected in LTWatchlist");
                return;
        }
        by_leading_term[clauses[id].row(new_watch).first_set()].push_back(id);
    }
};
} // namespace heuristics
