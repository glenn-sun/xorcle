#pragma once

/// @file phase.h
/// @brief Which side of a clause-derived decision equation to try first.

#include "algebra/equation.h"
#include "util/random.h"

namespace xorcle {

/// @brief The side of a decision wherever no saved trail chooses it.
enum class DecisionPhase {
    satisfy, ///< Decide the negation of the sampled row.
    falsify, ///< Decide the sampled row as it is stored.
    random ///< Choose per decision, from the phase RNG stream.
};

/// @brief Apply a decision phase in place to an equation in `falsify` form.
///
/// Consumes exactly one draw from `rng` under `DecisionPhase::random`, and none otherwise.
void apply_phase(Equation& eqn, DecisionPhase phase, Rng& rng);

} // namespace xorcle
