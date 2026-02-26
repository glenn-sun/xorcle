#include <cassert>

#include "heuristics/DecisionProcedure.h"
#include "util/DecisionUtils.h"
#include "util/RandomUtils.h"

namespace heuristics {

Equation DecisionProcedure::apply_phase(Equation eqn) const {
    switch (decision_phase) {
        case DecisionPhase::unsat:
            return eqn;
        case DecisionPhase::sat:
            eqn.flip(num_vars);
            return eqn;
        case DecisionPhase::random:
            if (util::random_int(2) == 1) {
                eqn.flip(num_vars);
            }
            return eqn;
    }
    return eqn;
}

void FixedUnit::reinitialize() {
    reset_stats();
    decision_basis.clear();
    for (std::size_t i = 0; i < num_vars; i++) {
        decision_basis.emplace_back(Equation::unit(num_vars + 1, i));
    }
}

Equation FixedUnit::make_decision(const RowSpace& trail_span, const std::vector<Clause>&) { 
    record_decision(false);
    return util::decide_from_basis(decision_basis, trail_span, num_vars);
}

void VsidsParams::bump_activity(const Equation& eqn) {
    activity_heap.increase_key(eqn, inc_amt);
    if (activity_heap.get_priority(eqn) > 1e100) {
        activity_heap.rescale(1e-100);
        inc_amt *= 1e-100;
    }
}

void VsidsUnit::reinitialize() {
    reset_stats();
    vsids_params.activity_heap.reset();
    if (unit_eqns.size() != num_vars) {
        unit_eqns.clear();
        unit_eqns.reserve(num_vars);
        for (std::size_t i = 0; i < num_vars; i++) {
            unit_eqns.emplace_back(Equation::unit(num_vars + 1, i));
        }
    }
    for (std::size_t i = 0; i < num_vars; i++) {
        const Equation& unit = unit_eqns[i];
        vsids_params.activity_heap.set_activity(unit, 0);
        vsids_params.activity_heap.insert(unit);
    }
    vsids_params.inc_amt = 1;
}

Equation VsidsUnit::make_decision(const RowSpace& trail_span, const std::vector<Clause>&) { 
    while (true) {
        std::optional<Equation> maybe_eqn = vsids_params.activity_heap.extract_max();
        if (!maybe_eqn.has_value()) {
            reinitialize();
            maybe_eqn = vsids_params.activity_heap.extract_max();
        }
        std::size_t f = trail_span.partial_reduce(*maybe_eqn);
        if ((f != bit::vector<>::npos) && (f != num_vars)) {
            record_decision(false);
            return *maybe_eqn;
        };
    }
}

void VsidsUnit::update_with_critical_equation(const Equation& eqn) {
    std::size_t leading_term = eqn.first_set();
    assert(leading_term < unit_eqns.size());
    vsids_params.bump_activity(unit_eqns[leading_term]);
}

void VsidsUnit::update_with_asserting_clause(const Clause& clause) {
    // Bump vars in final clause
    for (std::size_t i = 0; i < clause.rows(); ++i) {
        std::size_t var = clause.row(i).first_set();
        assert(var < unit_eqns.size());
        vsids_params.bump_activity(unit_eqns[var]);
    }

    // Decay activity by increasing increment
    vsids_params.inc_amt *= (1 / vsids_params.decay);

    vsids_params.activity_heap.rebuild();
}
} // namespace heuristics
