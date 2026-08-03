#pragma once

/// @file phase.h
/// @brief Which side of a clause-derived decision equation to try first.

#include "algebra/equation.h"
#include "util/random.h"

namespace xorcle {

/// @brief Phase selection for clause-based decisions.
///
/// A clause is stored in its linear negation, so a row @f$g_i = f_i +
/// \mathbf{1}@f$ describes where the clause literal @f$f_i@f$ is *false*.
/// Deciding a combination of those rows as-is drives the clause towards being
/// falsified; adding @f$\mathbf{1}@f$ first drives it towards being satisfied.
///
/// @note Only clause-based decisions consult this. `FixedLiterals` and
/// `VariableVsids` decide from a literal basis that carries no clause polarity,
/// and ignore the phase entirely.
enum class DecisionPhase {
    satisfy, ///< Decide the linear negation of the sampled row, so the clause becomes satisfied.
    falsify, ///< Decide the sampled row as it is stored, so the clause becomes falsified.
    random ///< Choose `satisfy` or `falsify` per decision, from the phase RNG stream.
};

/// @brief The phase used when no option selects one.
///
/// Fixed by the paper; it is the 0.1 tree's
/// `DecisionPhase::unsat` under its new name.
inline constexpr DecisionPhase default_decision_phase = DecisionPhase::falsify;

/// @brief Apply a decision phase to a decision equation, in place.
/// @param eqn The decision equation, in `falsify` form; modified in place.
/// @param phase The phase to apply.
/// @param false_equation A preallocated @f$\mathbf{1}@f$ of the same width as
/// `eqn`. Adding it flips the constant column, which is the linear negation
/// expressed as a whole-row operation rather than a per-bit flip.
/// @param rng The phase RNG stream; consumed by exactly one draw when `phase`
/// is `DecisionPhase::random`, and untouched otherwise.
/// @pre `eqn.size() == false_equation.size()` and both share a backend.
void apply_phase(Equation& eqn, DecisionPhase phase, const Equation& false_equation, Rng& rng);

} // namespace xorcle
