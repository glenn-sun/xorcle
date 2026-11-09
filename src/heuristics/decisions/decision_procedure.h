#pragma once

/// @file decision_procedure.h
/// @brief Choosing the next decision equation: the interface and the shared state.

#include <cstddef>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "heuristics/decisions/phase.h"
#include "heuristics/restarts/stabilize.h"
#include "util/index_set.h"
#include "util/random.h"
#include "util/stats.h"

namespace xorcle {

struct CsidsConfig;

enum class DecisionProcedureName {
    fixed_literals, ///< Debug baseline only.
    csids
};

/// @brief The levels propagation found to satisfy clauses, read against the
/// trail's levels as they now stand.
///
/// A level's stamp changes whenever a new decision replaces it, so a clause's
/// record holds exactly while its stamp matches.
class SatisfactionRecord {
public:
    /// @brief Knows of no satisfied clause.
    SatisfactionRecord() = default;

    /// @pre `level < level_stamps.size()`.
    SatisfactionRecord(std::span<const std::size_t> level_stamps, std::size_t level,
                       bool quiescent = false)
        : level_stamps_(level_stamps), level_(level), quiescent_(quiescent) {
        assert(level < level_stamps.size() && "the trail's level has no stamp");
    }

    /// @brief The shallowest level known to satisfy `clause`, when that level is
    /// still on the trail.
    std::optional<std::size_t> satisfying_level(const Clause& clause) const noexcept {
        if (clause.satisfied_stamp == 0 || clause.satisfied_level > level_ ||
            level_stamps_.empty() ||
            level_stamps_[clause.satisfied_level] != clause.satisfied_stamp) {
            return std::nullopt;
        }
        return clause.satisfied_level;
    }

    /// @brief The trail's decision level, empty for a record that knows nothing.
    std::optional<std::size_t> level() const noexcept {
        return level_stamps_.empty() ? std::nullopt : std::optional<std::size_t>(level_);
    }

    /// @brief Whether propagation last ran to quiescence, leaving no clause propagating.
    bool quiescent() const noexcept { return quiescent_; }

    /// @brief The stamp of every level up to the trail's, empty for a record that knows nothing.
    std::span<const std::size_t> level_stamps() const noexcept {
        return level_stamps_.empty() ? level_stamps_ : level_stamps_.first(level_ + 1);
    }

private:
    std::span<const std::size_t> level_stamps_;
    std::size_t level_ = 0;
    bool quiescent_ = false;
};

/// @brief What drawing decisions from clause intersections did, summed over the run.
struct IntersectionCounters {
    std::size_t decisions = 0; ///< Drawn from an intersection of two or more clauses.
    std::size_t intersected_clauses = 0; ///< Below the top, whose intersection was kept.
    /// @brief Of the top clause modulo the trail, counting under `span^±` the
    /// directions alone, as the plain form does.
    std::size_t dimension_before = 0;
    std::size_t dimension_after = 0; ///< Of the intersection drawn from, counted as `dimension_before`.
    std::size_t dimension_one = 0; ///< Decisions drawn from an intersection of dimension one.
    std::size_t empty_intersections = 0; ///< Open clauses below the top that were not kept.
    std::size_t skipped_intersections = 0; ///< Of those, the ones sharing no pivot column.
    std::size_t passed_satisfied_clauses = 0;
    std::size_t propagating_clauses = 0; ///< Found below the top and handed back.
    double time_s = 0.0;
};

/// @brief What choosing a decision found.
///
/// Exactly one member is engaged, or neither once nothing is left to decide
/// from.
struct DecisionResult {
    std::optional<Equation> equation;

    /// @brief A clause the trail has left at dimension one or zero, which
    /// propagates an equation or `0 = 1` rather than offering a decision.
    std::optional<ClauseId> propagating_clause;
};

struct DecisionCounters {
    std::size_t decisions = 0;
    /// @brief Variables the solver assigned when completing a model.
    std::size_t fallback_decisions = 0;
};

/// @brief Picks the equation the solver decides on next.
///
/// A decision `f` is legal when neither `f` nor `f + [0 = 1]` lies in the trail span.
///
/// @note `make_decision()` need not return a reduced equation; the solver re-reduces before
/// learning. Only the phase is observable, phase-then-reduce differing from the reverse.
class DecisionProcedure {
public:
    /// @brief The phase RNG stream is derived from `seed`.
    DecisionProcedure(EquationRepr repr, std::size_t num_vars, Seed seed);

    DecisionProcedure(const DecisionProcedure&) = delete;
    DecisionProcedure& operator=(const DecisionProcedure&) = delete;

    virtual ~DecisionProcedure() = default;

    /// @brief Reset for a fresh run, which a compaction is not.
    virtual void reinitialize() = 0;

    /// @brief Choose the next decision equation, of `num_vars() + 1` bits.
    /// @pre `trail_span.cols() == num_vars() + 1` and `trail_span.repr() == repr()`.
    /// @pre At least one legal decision remains: the trail leaves undetermined a
    /// variable no substitution has dropped.
    /// @pre `clause_active.size() == clauses.size()`.
    /// @param known What propagation already found satisfied, which spares the
    /// procedures that pass satisfied clauses over from checking them again.
    /// @return Both members empty when no clause offers a decision, which for
    /// CSIDS means every active clause is already satisfied. The procedures that
    /// decide a variable rather than a clause always return an equation.
    virtual DecisionResult make_decision(const RowSpace& trail_span,
                                         const std::vector<Clause>& clauses,
                                         const std::vector<std::uint8_t>& clause_active,
                                         const SatisfactionRecord& known = {}) = 0;

    /// @pre `clause.cols() == num_vars() + 1`, and every row has a variable leading bit.
    /// @note Called before `update_with_new_clause()` for the same clause, which is where
    /// CSIDS records the pending activity that call consumes.
    virtual void update_with_asserting_clause(const Clause& clause);

    /// @pre Ids are handed out consecutively, so `id` is at most the number of clauses
    /// already registered.
    virtual void update_with_new_clause(const Clause& clause, ClauseId id);

    virtual void update_with_resolved_clause(ClauseId id);

    /// @brief Note the trail of a non-root conflict, before the backjump, which
    /// the default ignores.
    /// @param level_stamps The stamp of every level of `trail`.
    virtual void update_with_conflict(const Trail& trail, std::span<const std::size_t> level_stamps);

    /// @brief Note that `replacement_id` subsumed `subsumed_id`, which is about to
    /// be deleted: the replacement inherits the larger of the two activities.
    /// @pre Both ids are already registered.
    virtual void update_with_subsumed_clause(ClauseId subsumed_id, ClauseId replacement_id);

    /// @brief Note that clause @p id was overwritten at the root by a narrowing,
    /// which the trail need not satisfy where it satisfied the clause.
    virtual void update_with_narrowed_clause(ClauseId id);

    /// @brief Note that the trail went back to decision level `level`, which
    /// leaves the candidates the discarded levels had ruled out to be offered again.
    virtual void update_with_backjump(std::size_t level);

    /// @brief Note that the solver decided an equation it chose itself, one level up.
    virtual void update_with_replayed_decision();

    /// @brief Adopt `mode`'s policies from the next decision on; the default ignores it.
    virtual void update_with_mode(SearchMode mode);

    /// @brief The clause the last decision came from, for the selector to prioritize.
    virtual std::optional<ClauseId> decision_clause_hint() const;

    /// @brief Note that a substitution rewrote the clauses and emptied the root
    /// trail, which the default ignores.
    /// @param dropped_col_idxs Every variable dropped since the last renumbering,
    /// over `num_vars() + 1` columns.
    virtual void update_with_substitution(const IndexSet& dropped_col_idxs,
                                          const std::vector<Clause>& clauses,
                                          const std::vector<std::uint8_t>& clause_active);

    /// @brief Remap procedure state into a renumbering of the variables.
    /// @pre `transform.old_num_vars() == num_vars()`.
    /// @note The default adopts the new variable count and nothing else.
    virtual void update_with_renumbering(const CompactionTransform& transform);

    const DecisionCounters& counters() const noexcept { return counters_; }

    /// @brief Add this procedure's counters to `stats`.
    virtual void add_to_stats(RunStats& stats) const;

    std::size_t num_vars() const noexcept { return num_vars_; }
    EquationRepr repr() const noexcept { return repr_; }

    void set_decision_phase(DecisionPhase phase) noexcept { decision_phase_ = phase; }

    DecisionPhase decision_phase() const noexcept { return decision_phase_; }

    /// @brief The run's seed, which every RNG stream of the run derives from.
    Seed base_seed() const noexcept { return base_seed_; }

protected:
    void record_decision() noexcept { ++counters_.decisions; }

public:
    /// @brief Record decisions the solver made itself, completing a model the
    /// clauses no longer constrain.
    void note_filled_decisions(std::size_t num_decisions);

protected:
    void reset_counters() noexcept { counters_ = {}; }

    void reset_phase_rng();

    /// @brief Apply the configured phase in place to an equation in `falsify` form.
    /// @pre `eqn` has `num_vars() + 1` bits and this procedure's representation.
    void apply_decision_phase(Equation& eqn) const { apply_decision_phase(eqn, decision_phase_); }

    /// @brief Apply `phase` in place, drawing from the phase stream as the configured one would.
    /// @pre As `apply_decision_phase(eqn)`.
    void apply_decision_phase(Equation& eqn, DecisionPhase phase) const;

    void set_num_vars(std::size_t num_vars);

    /// @pre `var < num_vars()`.
    Equation unit_equation(std::size_t var) const;

private:
    EquationRepr repr_;
    std::size_t num_vars_;
    Seed base_seed_;
    DecisionPhase decision_phase_ = default_decision_phase;

    mutable Rng phase_rng_;

    DecisionCounters counters_;

    /// @brief Preallocated, so `satisfy` is one whole-row op, not a per-bit flip.
    Equation false_equation_;
};

/// @brief Build the procedure named by @p name, deciding in @p phase; only CSIDS
/// reads @p csids.
std::unique_ptr<DecisionProcedure> make_decision_procedure(DecisionProcedureName name,
                                                           EquationRepr repr,
                                                           std::size_t num_vars, Seed seed,
                                                           DecisionPhase phase,
                                                           const CsidsConfig& csids);

} // namespace xorcle
