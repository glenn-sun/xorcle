#include "heuristics/ClauseSelector.h"

#include <algorithm>
#include <cassert>
#include <utility>

namespace heuristics {

ClauseSelector::ClauseSelector() : clauses(), clause_active() {}

std::size_t ClauseSelector::get_num_clauses() const { return clauses.size(); }

const std::vector<Clause>& ClauseSelector::get_clauses() const { return clauses; }

Clause& ClauseSelector::get_clause(ClauseId id) { return clauses[id]; }

bool ClauseSelector::is_clause_active(ClauseId id) const {
    return id < clause_active.size() && clause_active[id] != 0;
}

void ClauseSelector::set_clause_active(ClauseId id, bool active) {
    if (id >= clause_active.size()) {
        clause_active.resize(id + 1, 1);
    }
    clause_active[id] = active ? 1 : 0;
}

const std::vector<std::uint8_t>& ClauseSelector::active_flags() const { return clause_active; }

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

void ClauseSelector::update_with_compaction(const search::CompactionTransform& /*transform*/, const Trail& /*trail*/) {}

Cyclic::Cyclic(std::size_t /*num_vars*/) : ClauseSelector(), id(0), count(0) {}

std::optional<ClauseId> Cyclic::next() {
    if (clauses.empty()) {
        count = 0;
        return std::nullopt;
    }
    if (pending_restart_clause.has_value()) {
        ClauseId restart_id = pending_restart_clause.value();
        pending_restart_clause.reset();
        if (restart_id < clauses.size() && is_clause_active(restart_id)) {
            id = restart_id;
            count = 0;
        }
    }
    if (count >= clauses.size()) {
        count = 0;
        return std::nullopt;
    }
    for (std::size_t attempts = 0; attempts < clauses.size(); ++attempts) {
        std::size_t old_id = id;
        id = (id + 1) % clauses.size();
        ++count;
        if (is_clause_active(old_id)) {
            return old_id;
        }
        if (count >= clauses.size()) {
            count = 0;
            return std::nullopt;
        }
    }
    count = 0;
    return std::nullopt;
}

void Cyclic::add_clause(Clause clause) {
    ClauseSelector::add_clause(std::move(clause));
    id = clauses.size() - 1;
    count = 0;
}

void Cyclic::update_with_new_equation(const Equation& /*eqn*/, const Trail& /*trail*/) {
    count = 0;
}

void Cyclic::update_with_decision_clause(std::optional<ClauseId> clause_id) {
    if (clause_id.has_value() && clause_id.value() < clauses.size() && is_clause_active(clause_id.value())) {
        pending_restart_clause = clause_id;
    } else {
        pending_restart_clause.reset();
    }
}

void Cyclic::update_with_compaction(const search::CompactionTransform& /*transform*/, const Trail& /*trail*/) {
    if (clauses.empty()) {
        id = 0;
        count = 0;
        pending_restart_clause.reset();
        return;
    }
    if (id >= clauses.size()) {
        id %= clauses.size();
    }
    if (!is_clause_active(id)) {
        for (std::size_t i = 0; i < clauses.size(); ++i) {
            std::size_t cand = (id + i) % clauses.size();
            if (is_clause_active(cand)) {
                id = cand;
                break;
            }
        }
    }
    if (pending_restart_clause.has_value() && !is_clause_active(*pending_restart_clause)) {
        pending_restart_clause.reset();
    }
    count = 0;
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
    while (!stack.empty()) {
        ClauseId id = get_pop();
        if (is_clause_active(id)) {
            return id;
        }
    }
    return std::nullopt;
}

void LTWatchlist::add_clause(Clause clause) {
    ClauseId new_id = clauses.size();
    const bool active = new_id < clause_active.size() ? (clause_active[new_id] != 0) : true;
    if (clause.rows() > 1) {
        by_leading_term[clause.row(clause.watched0).first_set()].push_back(new_id);
        by_leading_term[clause.row(clause.watched1).first_set()].push_back(new_id);
        stack_set.push_back(false);
    } else {
        unit_clauses.push_back(new_id);
        stack_set.push_back(active);
        if (active) {
            stack.push_back(new_id);
        }
    }
    ClauseSelector::add_clause(std::move(clause));
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
    if (!is_clause_active(id)) {
        return;
    }
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

void LTWatchlist::update_with_compaction(const search::CompactionTransform& /*transform*/, const Trail& /*trail*/) {
    for (auto& bucket : by_leading_term) {
        bucket.clear();
    }
    unit_clauses.clear();

    std::vector<ClauseId> old_stack = stack;
    stack.clear();
    std::fill(stack_set.begin(), stack_set.end(), false);

    if (stack_set.size() < clauses.size()) {
        stack_set.resize(clauses.size(), false);
    }

    for (ClauseId id = 0; id < clauses.size(); ++id) {
        if (!is_clause_active(id)) {
            continue;
        }
        const Clause& clause = clauses[id];
        if (clause.rows() > 1) {
            by_leading_term[clause.row(clause.watched0).first_set()].push_back(id);
            by_leading_term[clause.row(clause.watched1).first_set()].push_back(id);
        } else {
            unit_clauses.push_back(id);
        }
    }

    for (ClauseId id : old_stack) {
        if (id < clauses.size() && is_clause_active(id)) {
            push(id);
        }
    }
    for (ClauseId id : unit_clauses) {
        push(id);
    }
}

} // namespace heuristics
