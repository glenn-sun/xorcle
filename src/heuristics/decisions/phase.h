#pragma once

/// @file phase.h
/// @brief Which side of a clause-derived decision equation to try first.

#include "algebra/equation.h"
#include "util/random.h"

namespace xorcle {

/// @brief Phase selection for clause-based decisions.
///
/// A clause is stored in its linear negation, so a row @f$g_i = f_i +
/// \mathbf{1}@f$ says where the literal @f$f_i@f$ is *false*. Deciding those
/// rows as stored drives the clause towards falsified; adding @f$\mathbf{1}@f$
/// first drives it towards satisfied.
///
/// @note `FixedLiterals` and `VariableVsids` decide from a literal basis that
/// carries no clause polarity, and ignore the phase entirely.
enum class DecisionPhase {
    satisfy, ///< Decide the linear negation of the sampled row, so the clause becomes satisfied.
    falsify, ///< Decide the sampled row as it is stored, so the clause becomes falsified.
    random ///< Choose `satisfy` or `falsify` per decision, from the phase RNG stream.
};

/// @brief The phase used when no option selects one.
inline constexpr DecisionPhase default_decision_phase = DecisionPhase::falsify;

/// @brief Apply a decision phase in place to an equation in `falsify` form.
/// `false_equation` is a preallocated @f$\mathbf{1}@f$, added to flip the
/// constant column in one whole-row operation; `rng` is consumed by exactly one
/// draw under `DecisionPhase::random` and untouched otherwise.
/// @pre `eqn.size() == false_equation.size()` and both share a representation.
void apply_phase(Equation& eqn, DecisionPhase phase, const Equation& false_equation, Rng& rng);

} // namespace xorcle
