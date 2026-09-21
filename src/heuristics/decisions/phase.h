#pragma once

/// @file phase.h
/// @brief Which side of a clause-derived decision equation to try first.

#include "algebra/equation.h"
#include "util/random.h"

namespace xorcle {

/// @note `FixedLiterals` decides from a literal basis that carries no clause polarity,
/// and ignores the phase entirely.
enum class DecisionPhase {
    satisfy, ///< Decide the linear negation of the sampled row.
    falsify, ///< Decide the sampled row as it is stored.
    random ///< Choose per decision, from the phase RNG stream.
};

inline constexpr DecisionPhase default_decision_phase = DecisionPhase::falsify;

/// @brief Apply a decision phase in place to an equation in `falsify` form.
///
/// Consumes exactly one draw from `rng` under `DecisionPhase::random`, and none
/// otherwise.
/// @pre `eqn.num_bits() == false_equation.num_bits()` and both share a representation.
void apply_phase(Equation& eqn, DecisionPhase phase, const Equation& false_equation, Rng& rng);

} // namespace xorcle
