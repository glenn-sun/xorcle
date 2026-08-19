#include "heuristics/decisions/phase.h"

namespace xorcle {

void apply_phase(Equation& eqn, DecisionPhase phase, const Equation& false_equation, Rng& rng) {
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
