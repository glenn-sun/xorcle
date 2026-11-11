#include "heuristics/decisions/phase.h"

#include <cassert>

namespace xorcle {

void apply_phase(Equation& eqn, DecisionPhase phase, Rng& rng) {
    switch (phase) {
        case DecisionPhase::falsify:
            return;
        case DecisionPhase::satisfy:
            eqn.negate();
            return;
        case DecisionPhase::random:
            if (uniform_below(rng, 2) == 1) {
                eqn.negate();
            }
            return;
    }
}

} // namespace xorcle
