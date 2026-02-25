#include "heuristics/CVsids.h"

#include <cassert>

#include "util/DecisionUtils.h"

namespace heuristics {
namespace {
/// @brief Shared decision implementation for CVsids-based strategies.
/// @param state Shared CVsids state.
/// @param num_vars Number of variables in the instance.
/// @param trail_span Current trail span.
/// @param clauses Current clause list.
/// @param decisions Decision counter to update.
/// @param fallback_decisions Fallback counter to update.
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
} // namespace heuristics
