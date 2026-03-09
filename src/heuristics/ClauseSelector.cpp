#include "heuristics/ClauseSelector.h"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace heuristics {

namespace {

void throw_clause_selector_error(const char* where, const std::string& message) {
    std::ostringstream out;
    out << "ClauseSelector invariant failure in " << where << ": " << message;
    throw std::logic_error(out.str());
}

std::size_t leading_term_bucket_or_throw(const Clause& clause,
                                         EquationId row_id,
                                         std::size_t bucket_count,
                                         const char* where) {
    if (row_id >= clause.rows()) {
        std::ostringstream msg;
        msg << "row index " << row_id << " out of range for clause with " << clause.rows() << " rows";
        throw_clause_selector_error(where, msg.str());
    }

    std::size_t leading_col = clause.row(row_id).first_set();
    if (leading_col >= bucket_count) {
        std::ostringstream msg;
        msg << "row " << row_id << " has invalid leading column " << leading_col
            << " for bucket count " << bucket_count;
        throw_clause_selector_error(where, msg.str());
    }
    return leading_col;
}

void validate_non_unit_watch_indices_or_throw(const Clause& clause, const char* where) {
    if (clause.rows() <= 1) {
        return;
    }
    if (clause.watched0 >= clause.rows() || clause.watched1 >= clause.rows()) {
        std::ostringstream msg;
        msg << "watched indices (" << clause.watched0 << ", " << clause.watched1
            << ") out of range for clause with " << clause.rows() << " rows";
        throw_clause_selector_error(where, msg.str());
    }
    if (clause.watched0 == clause.watched1) {
        std::ostringstream msg;
        msg << "non-unit clause reuses watched row " << clause.watched0;
        throw_clause_selector_error(where, msg.str());
    }
}

} // namespace

ClauseSelector::ClauseSelector() : clauses(), clause_active() {}

#ifndef NDEBUG
void ClauseSelector::debug_assert_storage_synchronized() const {
    assert(clauses.size() == clause_active.size());
}
#endif

std::size_t ClauseSelector::get_num_clauses() const { return clauses.size(); }

const std::vector<Clause>& ClauseSelector::get_clauses() const { return clauses; }

Clause& ClauseSelector::get_clause(ClauseId id) {
#ifndef NDEBUG
    debug_assert_storage_synchronized();
    assert(id < clauses.size());
#endif
    return clauses[id];
}

bool ClauseSelector::is_clause_active(ClauseId id) const {
#ifndef NDEBUG
    debug_assert_storage_synchronized();
    assert(id < clauses.size());
#endif
    return clause_active[id] != 0;
}

void ClauseSelector::set_clause_active(ClauseId id, bool active) {
    if (clauses.size() != clause_active.size()) {
        throw_clause_selector_error("set_clause_active", "clause storage and activity flags are desynchronized");
    }
    if (id >= clauses.size()) {
        std::ostringstream msg;
        msg << "clause id " << id << " out of range for " << clauses.size() << " clauses";
        throw std::out_of_range(msg.str());
    }

    clause_active[id] = active ? 1 : 0;
}

const std::vector<std::uint8_t>& ClauseSelector::active_flags() const {
#ifndef NDEBUG
    debug_assert_storage_synchronized();
#endif
    return clause_active;
}

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
#ifndef NDEBUG
    debug_assert_storage_synchronized();
    assert(id < clauses.size());
    assert(id < stack_set.size());
#endif
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
#ifndef NDEBUG
    assert(!stack.empty());
#endif
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
    if (stack_set.size() != clauses.size()) {
        throw_clause_selector_error("LTWatchlist::add_clause", "stack_set is not synchronized with clause storage");
    }

    ClauseId new_id = clauses.size();
    validate_non_unit_watch_indices_or_throw(clause, "LTWatchlist::add_clause");
    ClauseSelector::add_clause(std::move(clause));
    const Clause& stored_clause = clauses.back();

    stack_set.push_back(false);
    if (stored_clause.rows() > 1) {
        std::size_t watch0_bucket =
            leading_term_bucket_or_throw(stored_clause, stored_clause.watched0, by_leading_term.size(),
                                         "LTWatchlist::add_clause");
        std::size_t watch1_bucket =
            leading_term_bucket_or_throw(stored_clause, stored_clause.watched1, by_leading_term.size(),
                                         "LTWatchlist::add_clause");
        by_leading_term[watch0_bucket].push_back(new_id);
        by_leading_term[watch1_bucket].push_back(new_id);
    } else {
        unit_clauses.push_back(new_id);
        push(new_id);
    }

#ifndef NDEBUG
    debug_assert_storage_synchronized();
#endif
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
    const std::size_t leading_col = reduced_eqn.first_set();
#ifndef NDEBUG
    assert(leading_col < by_leading_term.size());
#endif

    for (std::size_t i = 0; i < leading_col; ++i) {
        if (trail.span().get_pivot_cols()[i]) {
            append(by_leading_term[i]);
        }
    }
    append(by_leading_term[leading_col]);
}

void LTWatchlist::update_with_new_watch(ClauseId id, EquationId old_watch, WatchType wt, const Trail& /*unused*/) {
#ifndef NDEBUG
    debug_assert_storage_synchronized();
    assert(id < clauses.size());
    assert(id < stack_set.size());
    assert(is_clause_active(id));
#endif
    const Clause& clause = clauses[id];
    validate_non_unit_watch_indices_or_throw(clause, "LTWatchlist::update_with_new_watch");
    std::size_t old_bucket =
        leading_term_bucket_or_throw(clause, old_watch, by_leading_term.size(), "LTWatchlist::update_with_new_watch");
    auto& old_bucket_rows = by_leading_term[old_bucket];
    std::erase(old_bucket_rows, id);
    EquationId new_watch = 0;
    switch (wt) {
        case WatchType::Zero:
            new_watch = clause.watched0;
            break;
        case WatchType::One:
            new_watch = clause.watched1;
            break;
        case WatchType::Sum:
            assert(false && "WatchType::Sum not expected in LTWatchlist");
            return;
    }
    std::size_t new_bucket =
        leading_term_bucket_or_throw(clause, new_watch, by_leading_term.size(), "LTWatchlist::update_with_new_watch");
    by_leading_term[new_bucket].push_back(id);
}

void LTWatchlist::update_with_compaction(const search::CompactionTransform& /*transform*/, const Trail& /*trail*/) {
    if (stack_set.size() != clauses.size()) {
        throw_clause_selector_error("LTWatchlist::update_with_compaction",
                                    "stack_set is not synchronized with clause storage");
    }

    for (auto& bucket : by_leading_term) {
        bucket.clear();
    }
    unit_clauses.clear();

    std::vector<ClauseId> old_stack = stack;
    stack.clear();
    std::fill(stack_set.begin(), stack_set.end(), false);

    for (ClauseId id = 0; id < clauses.size(); ++id) {
        if (!is_clause_active(id)) {
            continue;
        }
        const Clause& clause = clauses[id];
        if (clause.rows() > 1) {
            validate_non_unit_watch_indices_or_throw(clause, "LTWatchlist::update_with_compaction");
            std::size_t watch0_bucket =
                leading_term_bucket_or_throw(clause, clause.watched0, by_leading_term.size(),
                                             "LTWatchlist::update_with_compaction");
            std::size_t watch1_bucket =
                leading_term_bucket_or_throw(clause, clause.watched1, by_leading_term.size(),
                                             "LTWatchlist::update_with_compaction");
            by_leading_term[watch0_bucket].push_back(id);
            by_leading_term[watch1_bucket].push_back(id);
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

#ifndef NDEBUG
    debug_assert_storage_synchronized();
#endif
}

} // namespace heuristics
