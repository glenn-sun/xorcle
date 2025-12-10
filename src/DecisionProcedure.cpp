#include <cassert>
#include <algorithm>
#include "DecisionProcedure.h"

Equation decide_from_basis(const std::vector<Equation>& basis, const RowSpace& trail_span, std::size_t num_vars) {
    for (std::size_t i = 0; i < num_vars; ++i) {
        Equation eqn = basis[i];
        Equation eqn_reduced = trail_span.reduce_copy(eqn);
        Equation eqn_not_reduced = eqn_reduced;
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
        decision_basis.push_back(Equation::unit(num_vars + 1, i));
    }
}

Equation FixedUnit::make_decision(const RowSpace& trail_span) { 
    return decide_from_basis(decision_basis, trail_span, num_vars);
}

void RandomUnit::reinitialize() {
    decision_basis.clear();
    for (std::size_t i = 0; i < num_vars; i++) {
        decision_basis.push_back(Equation::unit(num_vars + 1, i));
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
    REFMatrix M;
    while (decision_basis.size() < num_vars) {
        Equation eqn = Equation::random(num_vars + 1);
        Equation eqn_reduced = M.reduce_copy(eqn);
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

void VsidsParams::bump_activity(std::size_t v) {
    activity[v] += inc_amt;
    if (activity[v] > 1e100) {
        for (std::size_t i = 0; i < activity.size(); i++)
            activity[i] *= 1e-100;
        inc_amt *= 1e-100;
    }

    // Update heap
    // TODO: Currently not needed because heap is completely 
    // recomputed at end of conflict analysis
}

void VsidsUnit::reinitialize() {
    vsids_params.activity.assign(num_vars, 0);
    vsids_params.order_heap = std::priority_queue<std::size_t, std::vector<std::size_t>, VsidsComparator>(VsidsComparator(vsids_params.activity));
    for (std::size_t i = 0; i < num_vars; i++) {
        vsids_params.order_heap.push(i);
    }
    vsids_params.inc_amt = 1;
}
Equation VsidsUnit::make_decision(const RowSpace& trail_span) { 
    Equation eqn, eqn_reduced, eqn_not_reduced;
    do {
        std::size_t next = vsids_params.order_heap.top();
        vsids_params.order_heap.pop();
        eqn = Equation::unit(num_vars + 1, next);
        eqn_reduced = trail_span.reduce_copy(eqn);
        eqn_not_reduced = eqn_reduced;
        eqn_not_reduced.flip(num_vars);
    } while (eqn_reduced.none() || eqn_not_reduced.none());
    return eqn;
}

void VsidsUnit::update_with_critical_equation(const Equation& eqn) {
    std::size_t leading_term = eqn.first_set();
    vsids_params.bump_activity(leading_term);
}

void VsidsUnit::update_with_asserting_clause(const Clause& clause) {
    // Bump vars in final clause
    for (std::size_t i = 0; i < clause.rows(); ++i) {
        std::size_t var = clause.row(i).first_set();
        vsids_params.bump_activity(var);
    }

    // Decay activity by increasing increment
    vsids_params.inc_amt *= (1 / vsids_params.decay);

    // Reset entire order_heap
    // TODO: Store a snapshot in the TrailSnapshot to avoid this
    vsids_params.order_heap = std::priority_queue<std::size_t, std::vector<std::size_t>, VsidsComparator>(VsidsComparator(vsids_params.activity));
    for (std::size_t i = 0; i < num_vars; i++) {
        vsids_params.order_heap.push(i);
    }
}