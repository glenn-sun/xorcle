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
    random, ///< Choose per decision, from the phase RNG stream.
    /// CSIDS chooses the side by its saved trails; a side they leave open is drawn as by `random`.
    saved,
    /// CSIDS chooses the side by its saved trails, consulted in an order that rotates with
    /// the conflicts; a side they leave open is the falsify side.
    rotate
};

inline constexpr DecisionPhase default_decision_phase = DecisionPhase::rotate;

/// @brief Whether CSIDS keeps saved trails under `phase`.
constexpr bool saves_trails(DecisionPhase phase) {
    return phase == DecisionPhase::saved || phase == DecisionPhase::rotate;
}

/// @brief Apply a decision phase in place to an equation in `falsify` form.
///
/// Consumes exactly one draw from `rng` under `DecisionPhase::random` and
/// `DecisionPhase::saved`, and none otherwise; `DecisionPhase::rotate` leaves `eqn` as it is.
/// @pre `eqn.num_bits() == false_equation.num_bits()` and both share a representation.
void apply_phase(Equation& eqn, DecisionPhase phase, const Equation& false_equation, Rng& rng);

} // namespace xorcle
