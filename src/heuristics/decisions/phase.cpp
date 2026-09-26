#include "heuristics/decisions/phase.h"

#include <cassert>

namespace xorcle {

void apply_phase(Equation& eqn, DecisionPhase phase, const Equation& false_equation, Rng& rng) {
    assert(eqn.num_bits() == false_equation.num_bits() &&
           "apply_phase: bit count mismatch with the preallocated 0 = 1");
    assert(eqn.repr() == false_equation.repr() &&
           "apply_phase: representation mismatch with the preallocated 0 = 1");

    switch (phase) {
        case DecisionPhase::falsify:
            return;
        case DecisionPhase::satisfy:
            eqn.plus_assign(false_equation);
            return;
        case DecisionPhase::random:
            if (random_int(rng, 2) == 1) {
                eqn.plus_assign(false_equation);
            }
            return;
    }
}

} // namespace xorcle
