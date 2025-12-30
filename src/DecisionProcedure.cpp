#include <cassert>
#include <algorithm>
#include "DecisionProcedure.h"

Equation decide_from_basis(const std::vector<Equation>& basis, const RowSpace& trail_span, std::size_t num_vars) {
    Equation eqn{num_vars}, eqn_reduced{num_vars}, eqn_not_reduced{num_vars};
    for (std::size_t i = 0; i < num_vars; ++i) {
        eqn.unsafe_copy(basis[i]);
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

void FixedUnit::reinitialize() {
    decision_basis.clear();
    for (std::size_t i = 0; i < num_vars; i++) {
        decision_basis.emplace_back(Equation::unit(num_vars + 1, i));
    }
}

Equation FixedUnit::make_decision(const RowSpace& trail_span) { 
    return decide_from_basis(decision_basis, trail_span, num_vars);
}

void RandomUnit::reinitialize() {
    decision_basis.clear();
    for (std::size_t i = 0; i < num_vars; i++) {
        decision_basis.emplace_back(Equation::unit(num_vars + 1, i));
    }

    std::random_device rd;
    std::mt19937 g(rd());

    std::shuffle(decision_basis.begin(), decision_basis.end(), g);
}
Equation RandomUnit::make_decision(const RowSpace& trail_span) { 
    return decide_from_basis(decision_basis, trail_span, num_vars);
}

void RandomEq::reinitialize() {
    decision_basis.clear();
    REFMatrix M(num_vars);
    Equation eqn{num_vars}, eqn_reduced{num_vars};
    while (decision_basis.size() < num_vars) {
        eqn.unsafe_copy(Equation::random(num_vars + 1));
        eqn_reduced.unsafe_copy(eqn);
        M.reduce(eqn_reduced);
        if (eqn_reduced != Equation::zeros(num_vars + 1) && 
            eqn_reduced != Equation::unit(num_vars + 1, num_vars)) {
            decision_basis.push_back(eqn_reduced);
            M.add_row(eqn_reduced);
        }
    }
}
Equation RandomEq::make_decision(const RowSpace& trail_span) { 
    return decide_from_basis(decision_basis, trail_span, num_vars);
}

void VsidsParams::bump_activity(const Equation& eqn) {
    activity_heap.increase_key(eqn, inc_amt);
    if (activity_heap.get_priority(eqn) > 1e100) {
        activity_heap.rescale(1e-100);
        inc_amt *= 1e-100;
    }
}

void VsidsUnit::reinitialize() {
    vsids_params.activity_heap.reset();
    for (std::size_t i = 0; i < num_vars; i++) {
        vsids_params.activity_heap.increase_key(Equation::unit(num_vars + 1, i), 0);
    }
    vsids_params.inc_amt = 1;
}

Equation VsidsUnit::make_decision(const RowSpace& trail_span) { 
    Equation eqn{num_vars}, eqn_reduced{num_vars}, eqn_not_reduced{num_vars};
    std::optional<Equation> maybe_eqn;
    do {
        maybe_eqn = vsids_params.activity_heap.extract_max();
        if (maybe_eqn.has_value()) {
            eqn = std::move(maybe_eqn.value());
        } else {
            reinitialize();
            eqn = vsids_params.activity_heap.extract_max().value();
        }
        eqn_reduced.unsafe_copy(eqn);
        trail_span.reduce(eqn_reduced);
        eqn_not_reduced.unsafe_copy(eqn_reduced);
        eqn_not_reduced.flip(num_vars);
    } while (eqn_reduced.none() || eqn_not_reduced.none());
    return eqn;
}

void VsidsUnit::update_with_critical_equation(const Equation& eqn) {
    std::size_t leading_term = eqn.first_set();
    Equation unit = Equation::unit(num_vars + 1, leading_term);
    vsids_params.bump_activity(unit);
}

void VsidsUnit::update_with_asserting_clause(const Clause& clause) {
    // Bump vars in final clause
    for (std::size_t i = 0; i < clause.rows(); ++i) {
        std::size_t var = clause.row(i).first_set();
        Equation unit = Equation::unit(num_vars + 1, var);
        vsids_params.bump_activity(unit);
    }

    // Decay activity by increasing increment
    vsids_params.inc_amt *= (1 / vsids_params.decay);
}