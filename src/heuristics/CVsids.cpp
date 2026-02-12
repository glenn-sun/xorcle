#include "heuristics/CVsids.h"

#include <cassert>
#include <numeric>

#include "util/DecisionUtils.h"

namespace heuristics {
namespace {
Equation make_decision_impl(CVsidsState& state,
                            std::size_t num_vars,
                            const RowSpace& trail_span,
                            const std::vector<Clause>& clauses,
                            bool sum_mode) {
    state.ensure_unit_eqns();
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
        state.restart_from_decision = true;

        if (!sum_mode) {
            return Equation(clause.row(clause.watched0));
        }

        while (true) {
            bit::vector<> coeffs = bit::vector<>::random(clause.rows());
            Equation candidate{num_vars};
            coeffs.if_set_call([&](std::size_t i) {
                candidate ^= clause.row(i);
            });

            Equation reduced{num_vars};
            assert(reduced.size() == candidate.size());
            reduced.unsafe_copy(candidate);
            trail_span.reduce(reduced);
            if (reduced.any()) {
                return reduced;
            }
        }
    }

    state.last_decision_clause.reset();
    state.restart_from_decision = false;
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

void CVsidsState::bind_clauses(const std::vector<Clause>* clauses_ptr) {
    clauses = clauses_ptr;
}

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
    if (clauses) {
        for (ClauseId id = 0; id < clauses->size(); ++id) {
            params.activity_heap.set_activity(id, 0);
            params.activity_heap.insert(id);
        }
    }
    params.inc_amt = 1;
    next_id = 0;
    count = 0;
    last_decision_clause.reset();
    restart_from_decision = false;
}

CVsidsDecision::CVsidsDecision(std::size_t n, std::shared_ptr<CVsidsState> shared_state)
    : DecisionProcedure(n), state(shared_state ? std::move(shared_state) : std::make_shared<CVsidsState>(n)) {
    reinitialize();
}

void CVsidsDecision::reinitialize() {
    state->reset_activity();
}

Equation CVsidsDecision::make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) {
    return make_decision_impl(*state, num_vars, trail_span, clauses, false);
}

void CVsidsDecision::update_with_asserting_clause(const Clause& /*clause*/) {
    state->params.inc_amt *= (1 / state->params.decay);
}

void CVsidsDecision::update_with_resolved_clause(ClauseId id) {
    if (state->clauses && id < state->clauses->size()) {
        state->params.bump_activity(id);
    }
}

Equation CVsidsSumDecision::make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) {
    return make_decision_impl(*state, num_vars, trail_span, clauses, true);
}

CVsidsSelector::CVsidsSelector(std::size_t n, std::shared_ptr<CVsidsState> shared_state)
    : ClauseSelector(), state(shared_state ? std::move(shared_state) : std::make_shared<CVsidsState>(n)) {
    state->bind_clauses(&clauses);
    state->reset_activity();
}

std::optional<ClauseId> CVsidsSelector::next() {
    if (clauses.empty()) {
        return std::nullopt;
    }
    if (state->restart_from_decision && state->last_decision_clause.has_value()) {
        state->next_id = state->last_decision_clause.value();
        state->count = 0;
        state->restart_from_decision = false;
    }
    if (state->count == clauses.size()) {
        state->count = 0;
        return std::nullopt;
    }
    ClauseId old_id = state->next_id;
    state->next_id = (state->next_id + 1) % clauses.size();
    ++state->count;
    return old_id;
}

void CVsidsSelector::add_starting_clause(const Clause& clause) {
    clauses.push_back(clause);
    ClauseId id = clauses.size() - 1;
    state->params.activity_heap.set_activity(id, 0);
    state->params.activity_heap.insert(id);
    state->next_id = clauses.size() - 1;
    state->count = 0;
}

void CVsidsSelector::add_asserting_clause(const Clause& clause, const Trail& /*trail*/, std::size_t /*jumpable_levels*/) {
    add_starting_clause(clause);
    state->params.activity_heap.rebuild();
    state->next_id = clauses.size() - 1;
    state->count = 0;
}

void CVsidsSelector::update_with_new_equation(const Equation& /*reduced_eqn*/, const Trail& /*trail*/) {
    state->count = 0;
}

void CVsidsSelector::update_with_decision(const Equation& /*reduced_eqn*/, const Trail& /*trail*/) {
    state->count = 0;
}

CVsids::CVsids(std::size_t n)
    : DecisionProcedure(n), ClauseSelector(), state(std::make_shared<CVsidsState>(n)) {
    state->bind_clauses(&clauses);
    reinitialize();
}

void CVsids::reinitialize() {
    state->reset_activity();
}

Equation CVsids::make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses_in) {
    return make_decision_impl(*state, num_vars, trail_span, clauses_in, false);
}

void CVsids::update_with_asserting_clause(const Clause& clause) {
    (void) clause;
    state->params.inc_amt *= (1 / state->params.decay);
}

void CVsids::update_with_resolved_clause(ClauseId id) {
    if (state->clauses && id < state->clauses->size()) {
        state->params.bump_activity(id);
    }
}

std::optional<ClauseId> CVsids::next() {
    if (clauses.empty()) {
        return std::nullopt;
    }
    if (state->restart_from_decision && state->last_decision_clause.has_value()) {
        state->next_id = state->last_decision_clause.value();
        state->count = 0;
        state->restart_from_decision = false;
    }
    if (state->count == clauses.size()) {
        state->count = 0;
        return std::nullopt;
    }
    ClauseId old_id = state->next_id;
    state->next_id = (state->next_id + 1) % clauses.size();
    ++state->count;
    return old_id;
}

void CVsids::add_starting_clause(const Clause& clause) {
    clauses.push_back(clause);
    ClauseId id = clauses.size() - 1;
    state->params.activity_heap.set_activity(id, 0);
    state->params.activity_heap.insert(id);
    state->next_id = clauses.size() - 1;
    state->count = 0;
}

void CVsids::add_asserting_clause(const Clause& clause, const Trail& /*trail*/, std::size_t /*jumpable_levels*/) {
    add_starting_clause(clause);
    state->params.activity_heap.rebuild();
    state->next_id = clauses.size() - 1;
    state->count = 0;
}

void CVsids::update_with_new_equation(const Equation& /*reduced_eqn*/, const Trail& /*trail*/) {
    state->count = 0;
}

void CVsids::update_with_decision(const Equation& /*reduced_eqn*/, const Trail& /*trail*/) {
    state->count = 0;
}

Equation CVsidsSum::make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses_in) {
    return make_decision_impl(*state, num_vars, trail_span, clauses_in, true);
}

} // namespace heuristics
