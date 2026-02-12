#include "heuristics/CVsids.h"

#include <cassert>

#include "util/DecisionUtils.h"

namespace heuristics {
namespace {
Equation make_decision_impl(CVsidsState& state,
                            std::size_t num_vars,
                            const RowSpace& trail_span,
                            const std::vector<Clause>& clauses,
                            std::size_t& decisions,
                            std::size_t& fallback_decisions) {
    state.ensure_unit_eqns();
    Equation candidate{num_vars};
    Equation reduced{num_vars};
    while (!state.params.activity_heap.empty()) {
        ClauseId id = state.params.activity_heap.peek_max();
        if (id >= clauses.size()) {
            state.params.activity_heap.extract_max();
            continue;
        }
        const Clause& clause = clauses[id];
        if (util::clause_satisfied_by_trail(trail_span, clause, num_vars)) {
            state.params.activity_heap.extract_max();
            continue;
        }
        state.last_decision_clause = id;
        if (util::sample_equation_from_clause(clause, trail_span, num_vars, candidate, reduced)) {
            ++decisions;
            return reduced;
        }
    }

    state.last_decision_clause.reset();
    ++decisions;
    ++fallback_decisions;
    return util::decide_from_units(trail_span, state.unit_eqns, num_vars);
}
} // namespace

void CVsidsParams::bump_activity(ClauseId id) {
    activity_heap.increase_key(id, inc_amt);
    if (activity_heap.get_priority(id) > 1e100) {
        activity_heap.rescale(1e-100);
        inc_amt *= 1e-100;
    }
}

CVsidsState::CVsidsState(std::size_t n)
    : eqn_false(Equation::unit(n + 1, n)), num_vars(n) {}

void CVsidsState::ensure_unit_eqns() {
    if (unit_eqns.size() != num_vars) {
        unit_eqns.clear();
        unit_eqns.reserve(num_vars);
        for (std::size_t i = 0; i < num_vars; i++) {
            unit_eqns.emplace_back(Equation::unit(num_vars + 1, i));
        }
    }
}

void CVsidsState::reset_activity() {
    params.activity_heap.reset();
    ensure_unit_eqns();
    for (ClauseId id = 0; id < num_clauses; ++id) {
        params.activity_heap.set_activity(id, 0);
        params.activity_heap.insert(id);
    }
    params.inc_amt = 1;
    last_decision_clause.reset();
}

CVsidsDecision::CVsidsDecision(std::size_t n, std::shared_ptr<CVsidsState> shared_state)
    : DecisionProcedure(n), state(shared_state ? std::move(shared_state) : std::make_shared<CVsidsState>(n)) {
    reinitialize();
}

void CVsidsDecision::reinitialize() {
    reset_stats();
    state->reset_activity();
}

Equation CVsidsDecision::make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) {
    return make_decision_impl(*state, num_vars, trail_span, clauses, decisions, fallback_decisions);
}

void CVsidsDecision::update_with_asserting_clause(const Clause& /*clause*/) {
    state->params.inc_amt *= (1 / state->params.decay);
    state->params.activity_heap.rebuild();
}

void CVsidsDecision::update_with_new_clause(const Clause& /*clause*/, ClauseId id) {
    if (id < state->num_clauses) {
        return;
    }
    for (ClauseId cur = state->num_clauses; cur <= id; ++cur) {
        state->params.activity_heap.set_activity(cur, 0);
        state->params.activity_heap.insert(cur);
    }
    state->num_clauses = id + 1;
}

void CVsidsDecision::update_with_resolved_clause(ClauseId id) {
    if (id < state->num_clauses) {
        state->params.bump_activity(id);
    }
}

std::optional<ClauseId> CVsidsDecision::decision_clause_hint() const {
    return state->last_decision_clause;
}

CVsids::CVsids(std::size_t n)
    : DecisionProcedure(n), ClauseSelector(), state(std::make_shared<CVsidsState>(n)) {
    reinitialize();
}

void CVsids::reinitialize() {
    reset_stats();
    state->reset_activity();
    next_id = 0;
    count = 0;
    pending_restart_clause.reset();
}

Equation CVsids::make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses_in) {
    return make_decision_impl(*state, num_vars, trail_span, clauses_in, decisions, fallback_decisions);
}

void CVsids::update_with_asserting_clause(const Clause& clause) {
    (void) clause;
    state->params.inc_amt *= (1 / state->params.decay);
    state->params.activity_heap.rebuild();
}

void CVsids::update_with_new_clause(const Clause& /*clause*/, ClauseId id) {
    if (id < state->num_clauses) {
        return;
    }
    for (ClauseId cur = state->num_clauses; cur <= id; ++cur) {
        state->params.activity_heap.set_activity(cur, 0);
        state->params.activity_heap.insert(cur);
    }
    state->num_clauses = id + 1;
}

void CVsids::update_with_resolved_clause(ClauseId id) {
    if (id < state->num_clauses) {
        state->params.bump_activity(id);
    }
}

std::optional<ClauseId> CVsids::decision_clause_hint() const {
    return state->last_decision_clause;
}

std::optional<ClauseId> CVsids::next() {
    if (clauses.empty()) {
        return std::nullopt;
    }
    if (pending_restart_clause.has_value()) {
        ClauseId restart_id = pending_restart_clause.value();
        pending_restart_clause.reset();
        if (restart_id < clauses.size()) {
            next_id = restart_id;
            count = 0;
        }
    }
    if (count == clauses.size()) {
        count = 0;
        return std::nullopt;
    }
    ClauseId old_id = next_id;
    next_id = (next_id + 1) % clauses.size();
    ++count;
    return old_id;
}

void CVsids::add_starting_clause(const Clause& clause) {
    clauses.push_back(clause);
    next_id = clauses.size() - 1;
    count = 0;
}

void CVsids::add_asserting_clause(const Clause& clause, const Trail& /*trail*/, std::size_t /*jumpable_levels*/) {
    add_starting_clause(clause);
}

void CVsids::update_with_new_equation(const Equation& /*reduced_eqn*/, const Trail& /*trail*/) {
    count = 0;
}

void CVsids::update_with_decision(const Equation& /*reduced_eqn*/, const Trail& /*trail*/) {
    count = 0;
}

void CVsids::update_with_decision_clause(std::optional<ClauseId> clause_id) {
    if (clause_id.has_value() && clause_id.value() < clauses.size()) {
        pending_restart_clause = clause_id;
    } else {
        pending_restart_clause.reset();
    }
}

} // namespace heuristics
