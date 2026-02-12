#include "CVsids.h"

void CVsidsParams::bump_activity(ClauseId id) {
    activity_heap.increase_key(id, inc_amt);
    if (activity_heap.get_priority(id) > 1e100) {
        activity_heap.rescale(1e-100);
        inc_amt *= 1e-100;
    }
}

CVsids::CVsids(std::size_t n) : DecisionProcedure(n), ClauseSelector(), eqn_false(Equation::unit(n + 1, n)) {
    reinitialize();
}

void CVsids::reinitialize() {
    params.activity_heap.reset();
    for (ClauseId id = 0; id < clauses.size(); ++id) {
        params.activity_heap.set_activity(id, 0);
        params.activity_heap.insert(id);
    }
    params.inc_amt = 1;
    next_id = 0;
    count = 0;
    last_decision_clause.reset();
    restart_from_decision = false;
}

bool CVsids::clause_satisfied(const RowSpace& trail_span, const Clause& clause) const {
    RowSpace combined = trail_span;
    for (std::size_t i = 0; i < clause.rows(); ++i) {
        combined.add_row(clause.row(i));
    }
    Equation test = eqn_false;
    combined.reduce(test);
    return test.none();
}

Equation CVsids::decide_from_units(const RowSpace& trail_span) const {
    Equation eqn{num_vars};
    Equation eqn_reduced{num_vars};
    Equation eqn_not_reduced{num_vars};
    for (std::size_t i = 0; i < num_vars; ++i) {
        eqn.unsafe_copy(Equation::unit(num_vars + 1, i));
        eqn_reduced.unsafe_copy(eqn);
        trail_span.reduce(eqn_reduced);
        eqn_not_reduced.unsafe_copy(eqn_reduced);
        eqn_not_reduced.flip(num_vars);
        if (eqn_reduced.any() && eqn_not_reduced.any()) {
            return eqn;
        }
    }
    assert(false);
}

Equation CVsids::make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses_in) {
    while (!params.activity_heap.empty()) {
        ClauseId id = params.activity_heap.peek_max();
        if (id >= clauses_in.size()) {
            params.activity_heap.extract_max();
            continue;
        }
        const Clause& clause = clauses_in[id];
        if (clause_satisfied(trail_span, clause)) {
            params.activity_heap.extract_max();
            continue;
        }
        last_decision_clause = id;
        restart_from_decision = true;
        return Equation(clause.row(clause.watched0));
    }

    last_decision_clause.reset();
    restart_from_decision = false;
    return decide_from_units(trail_span);
}

void CVsids::update_with_asserting_clause(const Clause& /*clause*/) {
    params.inc_amt *= (1 / params.decay);
}

void CVsids::update_with_resolved_clause(ClauseId id) {
    if (id < clauses.size()) {
        params.bump_activity(id);
    }
}

std::optional<ClauseId> CVsids::next() {
    if (clauses.empty()) {
        return std::nullopt;
    }
    if (restart_from_decision && last_decision_clause.has_value()) {
        next_id = last_decision_clause.value();
        count = 0;
        restart_from_decision = false;
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
    ClauseId id = clauses.size() - 1;
    params.activity_heap.set_activity(id, 0);
    params.activity_heap.insert(id);
    next_id = clauses.size() - 1;
    count = 0;
}

void CVsids::add_asserting_clause(const Clause& clause, const Trail& /*trail*/, std::size_t /*jumpable_levels*/) {
    add_starting_clause(clause);
    params.activity_heap.rebuild();
    next_id = clauses.size() - 1;
    count = 0;
}

void CVsids::update_with_new_equation(const Equation& /*reduced_eqn*/, const Trail& /*trail*/) {
    count = 0;
}

void CVsids::update_with_decision(const Equation& /*reduced_eqn*/, const Trail& /*trail*/) {
    count = 0;
}

Equation CVsidsSum::make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses_in) {
    while (!params.activity_heap.empty()) {
        ClauseId id = params.activity_heap.peek_max();
        if (id >= clauses_in.size()) {
            params.activity_heap.extract_max();
            continue;
        }
        const Clause& clause = clauses_in[id];
        if (clause_satisfied(trail_span, clause)) {
            params.activity_heap.extract_max();
            continue;
        }
        last_decision_clause = id;
        restart_from_decision = true;

        while (true) {
            bit::vector<> coeffs = bit::vector<>::random(clause.rows());
            Equation candidate{num_vars};
            coeffs.if_set_call([&](std::size_t i) {
                candidate ^= clause.row(i);
            });

            Equation reduced = candidate;
            trail_span.reduce(reduced);
            if (reduced.any()) {
                return reduced;
            }
        }
    }

    last_decision_clause.reset();
    restart_from_decision = false;
    return decide_from_units(trail_span);
}
