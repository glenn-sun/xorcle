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
    fixed_literals, ///< Debug baseline only.
    variable_vsids,
    csids
};

/// @brief Picks the equation the solver decides on next.
///
/// A decision `f` is legal when neither `f` nor `f + [0 = 1]` lies in the trail span.
///
/// @note `make_decision()` need not return a reduced equation; the solver re-reduces before
/// learning. Only the phase is observable, phase-then-reduce differing from the reverse.
class DecisionProcedure {
public:
    struct DecisionStats {
        std::size_t decisions = 0;
        /// @brief Decided from the literal basis, no clause having yielded one.
        std::size_t fallback_decisions = 0;
    };

    /// @brief The phase RNG stream is derived from `seed`.
    DecisionProcedure(EquationRepr repr, std::size_t num_vars, Seed seed);

    DecisionProcedure(const DecisionProcedure&) = delete;
    DecisionProcedure& operator=(const DecisionProcedure&) = delete;

    virtual ~DecisionProcedure() = default;

    /// @brief Reset for a fresh run, which the compaction remap is not.
    virtual void reinitialize() = 0;

    /// @brief Choose the next decision equation, of `num_vars() + 1` bits.
    /// @pre `trail_span.cols() == num_vars() + 1` and `trail_span.repr() == repr()`.
    /// @pre At least one legal decision remains, i.e. `trail_span.rows() < num_vars()`.
    virtual Equation make_decision(const RowSpace& trail_span,
                                   const std::vector<Clause>& clauses) = 0;

    /// @brief Note an equation resolved through during conflict analysis.
    /// @pre `eqn` has `num_vars() + 1` bits and a variable leading bit.
    virtual void update_with_critical_equation(const Equation& eqn);

    /// @pre `clause.cols() == num_vars() + 1`, and every row has a variable leading bit.
    /// @note Called before `update_with_new_clause()` for the same clause, which is where
    /// CSIDS records the pending activity that call consumes.
    virtual void update_with_asserting_clause(const Clause& clause);

    /// @pre Ids are handed out consecutively, so `id` is at most the number of clauses
    /// already registered.
    virtual void update_with_new_clause(const Clause& clause, ClauseId id);

    virtual void update_with_resolved_clause(ClauseId id);

    /// @brief The clause the last decision came from, for the selector to prioritize.
    virtual std::optional<ClauseId> decision_clause_hint() const;

    /// @brief How many of the lowest decision levels a restart may keep.
    ///
    /// The levels this procedure would rank ahead of the decision it is about to
    /// make, so that keeping them costs no search. `decision_clauses[k]` is the
    /// clause the decision at level `k + 1` came from, empty when the fallback
    /// basis supplied it.
    /// @return At most `decision_clauses.size()`. The default keeps nothing.
    /// @pre `trail_span` is not conflicting.
    virtual std::size_t reusable_levels(const RowSpace& trail_span,
                                        const std::vector<Clause>& clauses,
                                        const std::vector<std::optional<ClauseId>>& decision_clauses);

    /// @brief Remap procedure state into a compacted variable numbering.
    /// @pre `transform.old_num_vars() == num_vars()`.
    /// @note The default adopts the new active variable count and nothing else.
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

    void reset_phase_rng();

    /// @brief Apply the configured phase in place to an equation in `falsify` form.
    /// @pre `eqn` has `num_vars() + 1` bits and this procedure's representation.
    void apply_decision_phase(Equation& eqn) const;

    void set_num_vars(std::size_t num_vars);

    /// @pre `var < num_vars()`.
    Equation unit_equation(std::size_t var) const;

    Seed base_seed() const noexcept { return base_seed_; }

private:
    EquationRepr repr_;
    std::size_t num_vars_;
    Seed base_seed_;
    DecisionPhase decision_phase_ = default_decision_phase;

    mutable Rng phase_rng_;

    std::size_t decisions_ = 0;
    std::size_t fallback_decisions_ = 0;

    /// @brief Preallocated, so `satisfy` is one whole-row op, not a per-bit flip.
    Equation false_equation_;
};

} // namespace xorcle
