#pragma once

/// @file decision_procedure.h
/// @brief Choosing the next decision equation: the interface and the shared state.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "heuristics/decisions/phase.h"
#include "util/random.h"

namespace xorcle {

enum class DecisionProcedureName {
    fixed_literals, ///< Scan a fixed literal basis in variable order. Debug baseline only.
    variable_vsids, ///< VSIDS over per-variable literal equations.
    csids ///< Clause State Independent Decaying Sum, over clause activity.
};

/// @brief Picks the equation the solver decides on next. Any @f$f@f$ with
/// @f$f, f + \mathbf{1} \notin \mathrm{span}(U)@f$ is legal; `core/` applies the
/// decision and this picks which.
///
/// @note `make_decision()` need not return an equation reduced by the trail
/// span; the solver re-reduces before learning. `FixedLiterals` returns the raw
/// basis row, `VariableVsids` the reduced one, `Csids` the reduced one with the
/// phase applied — only the last is observable, since phase-then-reduce differs.
class DecisionProcedure {
public:
    struct DecisionStats {
        std::size_t decisions = 0; ///< Decisions made so far.
        std::size_t fallback_decisions = 0; ///< Of those, decisions that used a fallback path.
    };

    /// @brief The phase RNG stream is derived from `seed`.
    DecisionProcedure(EquationRepr repr, std::size_t num_vars, Seed seed);

    DecisionProcedure(const DecisionProcedure&) = delete;
    DecisionProcedure& operator=(const DecisionProcedure&) = delete;

    virtual ~DecisionProcedure() = default;

    /// @brief Reset for a fresh run — not a refill, not the compaction remap.
    virtual void reinitialize() = 0;

    /// @brief Choose the next decision equation, of `num_vars() + 1` bits.
    /// @pre `trail_span.cols() == num_vars() + 1` and `trail_span.repr() == repr()`.
    /// @pre At least one legal decision remains, i.e. `trail_span.rows() < num_vars()`.
    /// Every implementation asserts this: violating it costs `VariableVsids` a
    /// non-terminating refill loop rather than a wrong answer.
    virtual Equation make_decision(const RowSpace& trail_span,
                                   const std::vector<Clause>& clauses) = 0;

    /// @brief Note an equation resolved through during conflict analysis.
    /// @pre `eqn` has `num_vars() + 1` bits and a *variable* leading bit, i.e. it
    /// is neither @f$\mathbf{0}@f$ nor @f$\mathbf{1}@f$.
    virtual void update_with_critical_equation(const Equation& eqn);

    /// @brief Note that an asserting clause has been derived.
    /// @pre `clause.cols() == num_vars() + 1`, and every row has a variable
    /// leading bit.
    /// @note Called *before* `update_with_new_clause()` for the same clause, and
    /// the order matters: this is where CSIDS records the pending activity that
    /// call then consumes.
    virtual void update_with_asserting_clause(const Clause& clause);

    /// @brief Register a clause under its id in the store.
    /// @pre Ids are handed out consecutively, so `id` is at most the number of
    /// clauses already registered.
    virtual void update_with_new_clause(const Clause& clause, ClauseId id);

    virtual void update_with_resolved_clause(ClauseId id);

    /// @brief The clause the last decision came from, for the selector to prioritize.
    virtual std::optional<ClauseId> decision_clause_hint() const;

    /// @brief Remap procedure state into a compacted variable numbering. Not a
    /// fresh-run reset; the default adopts the new active variable count and nothing else.
    /// @pre `transform.old_num_vars() == num_vars()`.
    virtual void update_with_compaction(const CompactionTransform& transform,
                                        const RowSpace& trail_span,
                                        const std::vector<Clause>& clauses,
                                        const std::vector<std::uint8_t>& clause_active);

    virtual DecisionStats decision_stats() const;

    std::size_t num_vars() const noexcept { return num_vars_; }
    EquationRepr repr() const noexcept { return repr_; }

    void set_decision_phase(DecisionPhase phase) noexcept { decision_phase_ = phase; }

protected:
    void record_decision(bool used_fallback);
    void reset_stats() noexcept;

    /// @brief Restore the phase RNG stream to its seeded starting point.
    void reset_phase_rng();

    /// @brief Apply the configured phase in place to an equation in `falsify` form.
    /// @pre `eqn` has `num_vars() + 1` bits and this procedure's representation.
    void apply_decision_phase(Equation& eqn) const;

    /// @brief Adopt a new active variable count, resizing the shared scratch.
    void set_num_vars(std::size_t num_vars);

    /// @brief Build the unit equation @f$x_v@f$ over the active variables.
    /// @pre `var < num_vars()`; `var == num_vars()` would name the constant bit
    /// and silently build @f$\mathbf{1}@f$ instead.
    Equation unit_equation(std::size_t var) const;

    Seed base_seed() const noexcept { return base_seed_; }

private:
    EquationRepr repr_;
    std::size_t num_vars_;
    Seed base_seed_;
    DecisionPhase decision_phase_ = default_decision_phase;

    /// @brief Mutable so `apply_decision_phase()` can stay const: a random phase
    /// is a property of the run, not of the procedure's logical state.
    mutable Rng phase_rng_;

    std::size_t decisions_ = 0;
    std::size_t fallback_decisions_ = 0;

    /// @brief Preallocated, so `satisfy` is one whole-row op, not a per-bit flip.
    Equation false_equation_;
};

} // namespace xorcle
