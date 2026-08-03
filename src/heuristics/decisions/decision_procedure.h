#pragma once

/// @file decision_procedure.h
/// @brief Choosing the next decision equation: the interface every decision
/// procedure implements, plus the state they all share.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "algebra/algebra.h"
#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "heuristics/decisions/phase.h"
#include "util/random.h"

namespace xorcle {

/// @brief Available decision procedure implementations.
enum class DecisionProcedureName {
    fixed_literals, ///< Scan a fixed literal basis in variable order. Debug baseline only.
    variable_vsids, ///< VSIDS over per-variable literal equations.
    csids ///< Clause State Independent Decaying Sum, over clause activity.
};

/// @brief Picks the equation the solver decides on next.
///
/// Algorithm 1 says only "decide any @f$f@f$ with @f$f, f + \mathbf{1} \notin
/// \mathrm{span}(U)@f$". That is a free choice the algorithm deliberately
/// leaves open, so `core/` applies the decision and this picks it.
///
/// The base class owns what every implementation needs: the active variable
/// count, the phase and its RNG stream, the decision counters, and a
/// preallocated @f$\mathbf{1}@f$ for `apply_phase()`.
///
/// @note The equation returned by `make_decision()` is not required to be
/// reduced by the trail span; the solver re-reduces it before learning. The
/// implementations differ here — `FixedLiterals` returns the raw basis row,
/// `VariableVsids` the reduced one, `Csids` the reduced one with the phase
/// applied — and only the last of those is observable, because applying the
/// phase to a reduced equation is not the same as applying it before reducing.
class DecisionProcedure {
public:
    /// @brief Decision statistics snapshot.
    struct DecisionStats {
        std::size_t decisions = 0; ///< Decisions made so far.
        std::size_t fallback_decisions = 0; ///< Of those, decisions that used a fallback path.
    };

    /// @brief Construct a decision procedure for a given active variable count.
    /// @param backend Storage representation to use for the equations it builds.
    /// @param num_vars Number of active variable columns.
    /// @param seed Base solver seed; the phase RNG stream is derived from it.
    DecisionProcedure(Algebra backend, std::size_t num_vars, Seed seed);

    DecisionProcedure(const DecisionProcedure&) = delete;
    DecisionProcedure& operator=(const DecisionProcedure&) = delete;

    /// @brief Virtual destructor for interface use.
    virtual ~DecisionProcedure() = default;

    /// @brief Reset procedure-owned state for a fresh solver run.
    /// @note This is a fresh-run reset, not a mid-solve refill and not the
    /// compaction remap; those are `refill` paths internal to the
    /// implementations and `update_with_compaction()` respectively.
    virtual void reinitialize() = 0;

    /// @brief Choose the next decision equation.
    /// @param trail_span The current @f$\mathrm{span}(U)@f$.
    /// @param clauses The clause database, in clause-id order.
    /// @return A decision equation of width `num_vars() + 1`.
    /// @pre At least one legal decision remains, i.e. `trail_span.rows() < num_vars()`.
    virtual Equation make_decision(const RowSpace& trail_span,
                                   const std::vector<Clause>& clauses) = 0;

    /// @brief Note an equation resolved through during conflict analysis.
    /// @param eqn The trail row the resolution step consumed.
    virtual void update_with_critical_equation(const Equation& eqn);

    /// @brief Note that an asserting clause has been derived.
    /// @param clause The asserting clause, before it is added to the database.
    /// @note Called *before* `update_with_new_clause()` for the same clause, and
    /// the order matters: this is where CSIDS records the pending activity that
    /// `update_with_new_clause()` then consumes.
    virtual void update_with_asserting_clause(const Clause& clause);

    /// @brief Note a clause entering the database under a given id.
    /// @param clause The clause being added.
    /// @param id The id it is being added under.
    virtual void update_with_new_clause(const Clause& clause, ClauseId id);

    /// @brief Note that a clause was used as a reason during conflict analysis.
    /// @param id The reason clause.
    virtual void update_with_resolved_clause(ClauseId id);

    /// @brief The clause the last decision came from, if any.
    /// @return A clause id the selector should prioritize, or `std::nullopt`.
    virtual std::optional<ClauseId> decision_clause_hint() const;

    /// @brief Remap procedure state into a compacted variable space.
    /// @param transform The compaction step that was applied.
    /// @param trail_span The compacted trail span.
    /// @param clauses The compacted clause database.
    /// @param clause_active Per-clause active flags, in clause-id order.
    /// @note This is a remap, not a fresh-run reset. The default implementation
    /// adopts the new active width and nothing else.
    virtual void update_with_compaction(const CompactionTransform& transform,
                                        const RowSpace& trail_span,
                                        const std::vector<Clause>& clauses,
                                        const std::vector<std::uint8_t>& clause_active);

    /// @brief Current decision statistics.
    virtual DecisionStats decision_stats() const;

    /// @brief Number of active variable columns.
    std::size_t num_vars() const noexcept { return num_vars_; }

    /// @brief Which backend the equations it builds use.
    Algebra backend() const noexcept { return backend_; }

    /// @brief Set the phase applied to clause-based decisions.
    void set_decision_phase(DecisionPhase phase) noexcept { decision_phase_ = phase; }

    /// @brief The phase applied to clause-based decisions.
    DecisionPhase decision_phase() const noexcept { return decision_phase_; }

protected:
    /// @brief Count one decision.
    /// @param used_fallback Whether the decision came from a fallback path.
    void record_decision(bool used_fallback);

    /// @brief Zero the decision counters.
    void reset_stats() noexcept;

    /// @brief Restore the phase RNG stream to its seeded starting point.
    void reset_phase_rng();

    /// @brief Apply the configured phase to a decision equation, in place.
    /// @param eqn A decision equation of width `num_vars() + 1`, in `falsify` form.
    void apply_decision_phase(Equation& eqn) const;

    /// @brief Adopt a new active variable count, resizing the shared scratch.
    /// @param num_vars The new number of active variable columns.
    void set_num_vars(std::size_t num_vars);

    /// @brief Build the unit equation @f$x_v@f$ over the active variables.
    /// @param var The variable to isolate.
    Equation unit_equation(std::size_t var) const;

    /// @brief The base solver seed, so derived streams can be re-derived.
    Seed base_seed() const noexcept { return base_seed_; }

    /// @brief A preallocated @f$\mathbf{1}@f$ over the active variables.
    const Equation& false_equation() const noexcept { return false_equation_; }

private:
    Algebra backend_;
    std::size_t num_vars_;
    Seed base_seed_;
    DecisionPhase decision_phase_ = default_decision_phase;

    /// @brief Mutable so `apply_decision_phase()` can stay const, matching the
    /// fact that a random phase is a property of the run rather than of the
    /// procedure's logical state.
    mutable Rng phase_rng_;

    std::size_t decisions_ = 0;
    std::size_t fallback_decisions_ = 0;

    /// @brief Preallocated @f$\mathbf{1}@f$, so applying the `satisfy` phase is
    /// one whole-row operation instead of a per-bit flip.
    Equation false_equation_;
};

} // namespace xorcle
