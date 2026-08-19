#include "heuristics/selection/clause_selector.h"

#include <cassert>
#include <utility>

namespace xorcle {

#ifndef NDEBUG
void ClauseSelector::debug_assert_database_synchronized() const {
    assert(clauses_.size() == clause_active_.size() &&
           "clause database and activity flags are desynchronized");
}
#endif

Clause& ClauseSelector::clause(ClauseId id) {
#ifndef NDEBUG
    debug_assert_database_synchronized();
#endif
    assert(id < clauses_.size());
    return clauses_[id];
}

const Clause& ClauseSelector::clause(ClauseId id) const {
#ifndef NDEBUG
    debug_assert_database_synchronized();
#endif
    assert(id < clauses_.size());
    return clauses_[id];
}

bool ClauseSelector::is_clause_active(ClauseId id) const {
#ifndef NDEBUG
    debug_assert_database_synchronized();
#endif
    assert(id < clauses_.size());
    return clause_active_[id] != 0;
}

void ClauseSelector::set_clause_active(ClauseId id, bool active) {
#ifndef NDEBUG
    debug_assert_database_synchronized();
#endif
    assert(id < clauses_.size());
    clause_active_[id] = active ? 1 : 0;
}

std::optional<ClauseSelection> ClauseSelector::next_with_hint() {
    const std::optional<ClauseId> id = next();
    if (!id.has_value()) {
        return std::nullopt;
    }
    return ClauseSelection{*id, std::nullopt};
}

void ClauseSelector::add_starting_clause(Clause clause) {
    add_clause(std::move(clause));
}

void ClauseSelector::add_asserting_clause(Clause clause, const Trail&, std::size_t) {
    add_clause(std::move(clause));
}

void ClauseSelector::update_with_propagation(const Equation& reduced_eqn, const Trail& trail) {
    update_with_new_equation(reduced_eqn, trail);
}

void ClauseSelector::update_with_decision(const Equation& reduced_eqn, const Trail& trail) {
    update_with_new_equation(reduced_eqn, trail);
}

void ClauseSelector::update_with_decision_clause(std::optional<ClauseId>) {}

void ClauseSelector::update_with_new_watch(ClauseId, ClauseRowIndex, WatchType, const Trail&) {}

void ClauseSelector::update_with_compaction(const CompactionTransform&, const Trail&) {}

void ClauseSelector::add_clause(Clause clause) {
    clauses_.push_back(std::move(clause));
    clause_active_.push_back(1);
}

void ClauseSelector::update_with_new_equation(const Equation&, const Trail&) {}

} // namespace xorcle
