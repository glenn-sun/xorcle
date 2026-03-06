#include "heuristics/ClauseSelector.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace heuristics {

ClauseSelector::ClauseSelector() : clauses() {}

std::size_t ClauseSelector::get_num_clauses() const { return clauses.size(); }

const std::vector<Clause>& ClauseSelector::get_clauses() const { return clauses; }

Clause& ClauseSelector::get_clause(ClauseId id) { return clauses[id]; }

std::optional<ClauseSelection> ClauseSelector::next_with_hint() {
    auto id = next();
    if (!id.has_value()) {
        return std::nullopt;
    }
    return ClauseSelection{id.value(), std::nullopt};
}

void ClauseSelector::add_starting_clause(Clause clause) { add_clause(std::move(clause)); }

void ClauseSelector::add_asserting_clause(Clause clause, const Trail& /*trail*/, std::size_t /*jumpable_levels*/) {
    add_clause(std::move(clause));
}

void ClauseSelector::update_with_propagation(const Equation& reduced_eqn, const Trail& trail) {
    update_with_new_equation(reduced_eqn, trail);
}

void ClauseSelector::update_with_decision(const Equation& reduced_eqn, const Trail& trail) {
    update_with_new_equation(reduced_eqn, trail);
}

void ClauseSelector::update_with_decision_clause(std::optional<ClauseId> /*clause_id*/) {}

void ClauseSelector::update_with_new_watch(ClauseId /*id*/, EquationId /*old_watch*/, WatchType /*wt*/, const Trail& /*trail*/) {}

Cyclic::Cyclic(std::size_t /*num_vars*/) : ClauseSelector(), id(0), count(0) {}

std::optional<ClauseId> Cyclic::next() {
    if (pending_restart_clause.has_value()) {
        ClauseId restart_id = pending_restart_clause.value();
        pending_restart_clause.reset();
        if (restart_id < clauses.size()) {
            id = restart_id;
            count = 0;
        }
    }
    if (count == clauses.size()) {
        count = 0;
        return std::nullopt;
    }
    std::size_t old_id = id;
    id = (id + 1) % clauses.size();
    ++count;
    return old_id;
}

void Cyclic::add_clause(Clause clause) {
    clauses.push_back(std::move(clause));
    id = clauses.size() - 1;
    count = 0;
}

void Cyclic::update_with_new_equation(const Equation& /*eqn*/, const Trail& /*trail*/) {
    count = 0;
}

void Cyclic::update_with_decision_clause(std::optional<ClauseId> clause_id) {
    if (clause_id.has_value() && clause_id.value() < clauses.size()) {
        pending_restart_clause = clause_id;
    } else {
        pending_restart_clause.reset();
    }
}

LTWatchlist::LTWatchlist(std::size_t num_vars)
    : ClauseSelector(), by_leading_term(num_vars + 1) {}

void LTWatchlist::push(ClauseId id) {
    if (!stack_set[id]) {
        stack.push_back(id);
        stack_set[id] = true;
    }
}

void LTWatchlist::append(const std::vector<ClauseId>& ids) {
    for (ClauseId id : ids) {
        push(id);
    }
}

ClauseId LTWatchlist::get_pop() {
    ClauseId id = stack.back();
    stack.pop_back();
    stack_set[id] = false;
    return id;
}

std::optional<ClauseId> LTWatchlist::next() {
    if (stack.empty()) {
        return std::nullopt;
    }
    return get_pop();
}

void LTWatchlist::add_clause(Clause clause) {
    if (clause.rows() > 1) {
        by_leading_term[clause.row(clause.watched0).first_set()].push_back(clauses.size());
        by_leading_term[clause.row(clause.watched1).first_set()].push_back(clauses.size());
        stack_set.push_back(false);
    } else {
        unit_clauses.push_back(clauses.size());
        stack_set.push_back(true);
        stack.push_back(clauses.size());
    }
    clauses.push_back(std::move(clause));
}

void LTWatchlist::add_asserting_clause(Clause clause, const Trail& /*unused*/, std::size_t /*unused*/) {
    add_clause(std::move(clause));

    stack.clear();
    std::fill(stack_set.begin(), stack_set.end(), false);
    append(unit_clauses);
    push(clauses.size() - 1);
}

void LTWatchlist::update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) {
    // Because of linear combinations, need to check all lists corresponding
    // to reducible leading terms in position at most reduced.first_set()

    for (std::size_t i = 0; i < reduced_eqn.first_set(); ++i) {
        if (trail.span().get_pivot_cols()[i]) {
            append(by_leading_term[i]);
        }
    }
    append(by_leading_term[reduced_eqn.first_set()]);
}

void LTWatchlist::update_with_new_watch(ClauseId id, EquationId old_watch, WatchType wt, const Trail& /*unused*/) {
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

} // namespace heuristics
