#pragma once

/// @file csids.h
/// @brief CSIDS: Clause State Independent Decaying Sum, the decision procedure.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/equation_pool.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_database.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "heuristics/decisions/clause_decisions.h"
#include "heuristics/decisions/phase.h"
#include "heuristics/decisions/saved_trails.h"
#include "heuristics/restarts/stabilize.h"
#include "util/index_set.h"
#include "util/max_heap.h"
#include "util/random.h"
#include "util/stats.h"

namespace xorcle {

/// @brief The levels propagation found to satisfy clauses, read against the
/// trail's levels as they now stand.
///
/// A level's stamp changes whenever a new decision replaces it, so a clause's
/// record holds exactly while its stamp matches.
class SatisfactionRecord {
public:
    /// @brief Knows of no satisfied clause.
    SatisfactionRecord() = default;

    /// @param records Every clause's record, indexed by clause id.
    /// @pre `level < level_stamps.size()`.
    SatisfactionRecord(std::span<const ClauseRecord> records,
                       std::span<const std::size_t> level_stamps, std::size_t level,
                       bool quiescent = false)
        : records_(records), level_stamps_(level_stamps), level_(level), quiescent_(quiescent) {
        assert(level < level_stamps.size() && "the trail's level has no stamp");
    }

    /// @brief The shallowest level known to satisfy the clause of `record`, when
    /// that level is still on the trail.
    std::optional<std::size_t> satisfying_level(const ClauseRecord& record) const noexcept {
        if (record.satisfied_stamp == 0 || record.satisfied_level > level_ ||
            level_stamps_.empty() ||
            level_stamps_[record.satisfied_level] != record.satisfied_stamp) {
            return std::nullopt;
        }
        return record.satisfied_level;
    }

    /// @brief `satisfying_level()` of the clause @p id.
    std::optional<std::size_t> satisfying_level(ClauseId id) const noexcept {
        if (level_stamps_.empty()) {
            return std::nullopt;
        }
        return satisfying_level(records_[id]);
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
    std::span<const ClauseRecord> records_;
    std::span<const std::size_t> level_stamps_;
    std::size_t level_ = 0;
    bool quiescent_ = false;
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
};


/// @brief Which span of each clause's linear negation, modulo the trail span U,
/// CSIDS intersects and draws a decision from.
enum class CsidsSpan {
    plain, ///< `span(neg(C), U)`, deciding in the configured phase.
    /// `span^±(neg(C), U)`, which also holds `0 = 1`: a decision is a direction
    /// the trail does not settle, whose `falsify` side lies in the top clause's
    /// `span(neg(C), U)`.
    plus_minus
};

/// @brief Tunable CSIDS parameters.
struct CsidsConfig {
    double initial_activity; ///< A_0, an asserting clause's activity on entry.

    double decay; ///< `1` never decays, `0` keeps only the newest activity.
    double focused_decay; ///< The decay in focused mode.
    double bump; ///< δ, what one resolution step adds to a clause's activity.

    /// @brief The open clauses, from the top of the heap down, whose linear
    /// negations a decision is drawn from the intersection of; `1` is the top
    /// clause alone.
    std::size_t intersection_depth;

    CsidsSpan span;

    DecisionPhase phase;

    PhaseSavingConfig phase_saving;
};

/// @brief Decides from the highest-activity clause the trail has not satisfied.
///
/// Activity is per clause: bumped each time conflict analysis uses it, decayed
/// at every conflict. See arXiv:2605.15002 for details.
///
/// A decision `f` is legal when neither `f` nor `f + [0 = 1]` lies in the trail span.
class Csids {
public:
    /// @brief Every RNG stream derives from `seed`; the mode is stable.
    /// @pre `config.decay` and `config.focused_decay` lie in `[0, 1]`, and
    /// `config.initial_activity` and `config.bump` are non-negative.
    Csids(EquationRepr repr, std::size_t num_vars, Seed seed, const CsidsConfig& config);

    Csids(const Csids&) = delete;
    Csids& operator=(const Csids&) = delete;

    /// @brief Reset for a fresh run, which a compaction is not: the RNG streams,
    /// activity and the counters.
    void reinitialize();

    /// @brief Choose the next decision equation, of `num_vars() + 1` bits, which need
    /// not be reduced.
    /// @pre `trail_span.cols() == num_vars() + 1` and `trail_span.repr() == repr()`.
    /// @pre At least one legal decision remains: the trail leaves undetermined a
    /// variable no substitution has dropped.
    /// @pre `clause_active.size() == clauses.size()`.
    /// @param known What propagation already found satisfied, which spares
    /// checking those clauses again.
    /// @return Both members empty when every active clause is already satisfied.
    DecisionResult make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses,
                                 const std::vector<std::uint8_t>& clause_active,
                                 const SatisfactionRecord& known = {});

    /// @brief Record the activity the next new clause will enter at, then decay.
    /// @note Called before `update_with_new_clause()` for the same clause.
    void update_with_asserting_clause(const Clause& clause);

    /// @brief Register a clause id, consuming any pending learned activity.
    /// @pre `id` is the number of clauses already registered.
    void update_with_new_clause(const Clause& clause, ClauseId id);

    /// @brief Bump a clause conflict analysis used.
    /// @pre The clause is registered.
    void update_with_resolved_clause(ClauseId id);

    /// @brief Save the trail of a non-root conflict, before the backjump, for phase saving.
    /// @param level_stamps The stamp of every level of `trail`.
    void update_with_conflict(const Trail& trail, std::span<const std::size_t> level_stamps);

    /// @brief Put the clauses found satisfied above `level` back in the heap.
    /// @pre `level` is at most the level of the trail's last decision.
    void update_with_backjump(std::size_t level);

    /// @brief Note that the solver decided an equation it chose itself, one level up.
    void update_with_replayed_decision() { ++level_; }

    /// @brief Decay by the mode's decay, and save phases by the mode's source.
    void update_with_mode(SearchMode mode);

    /// @brief Note that `replacement_id` subsumed `subsumed_id`, which is about to
    /// be deleted: the replacement inherits the larger of the two activities.
    /// @pre Both ids are already registered.
    void update_with_subsumed_clause(ClauseId subsumed_id, ClauseId replacement_id);

    /// @brief Put clause @p id, overwritten at the root by a narrowing, back in the heap.
    void update_with_narrowed_clause(ClauseId id);

    /// @brief The clause's activity, or `0` where it has none.
    double clause_activity(ClauseId id) const;

    /// @brief The clause the last decision came from, for the selector to prioritize.
    std::optional<ClauseId> decision_clause_hint() const;

    /// @brief Note that a substitution rewrote the clauses and emptied the root trail.
    /// @param root_span The root rows the substitution substituted.
    /// @param dropped_col_idxs Every variable dropped since the last renumbering,
    /// over `num_vars() + 1` columns.
    void update_with_substitution(const RowSpace& root_span, const IndexSet& dropped_col_idxs,
                                  const std::vector<Clause>& clauses,
                                  const std::vector<std::uint8_t>& clause_active);

    /// @pre `transform.old_num_vars() == num_vars()`.
    void update_with_renumbering(const CompactionTransform& transform);

    const DecisionCounters& counters() const noexcept { return counters_; }

    void add_to_stats(RunStats& stats) const;

    std::size_t num_vars() const noexcept { return num_vars_; }
    EquationRepr repr() const noexcept { return repr_; }
    DecisionPhase decision_phase() const noexcept { return decision_phase_; }

    /// @brief The run's seed, which every RNG stream of the run derives from.
    Seed base_seed() const noexcept { return base_seed_; }

private:
    /// @brief Apply the configured phase in place to an equation in `falsify` form.
    void apply_decision_phase(Equation& eqn) const;

    /// @brief Intersect the open top clause's linear negation, modulo the trail,
    /// greedily with those of the open clauses below it, keeping each
    /// intersection that stays outside the trail span.
    /// @return A decision from the intersection, or a clause below the top that the
    /// trail leaves propagating.
    /// @pre `top_id` is the heap's maximum, the trail leaves it open, and it
    /// was the last clause checked.
    DecisionResult decide_from_intersection(
        ClauseId top_id, const RowSpace& trail_span, const std::vector<Clause>& clauses,
        const std::vector<std::uint8_t>& clause_active, const SatisfactionRecord& known);


    /// @brief What the trail leaves of the heap's top clause; a satisfied one
    /// comes off the heap until a backjump discards the level found to satisfy it.
    /// @pre `id` is the heap's maximum and names `clause`.
    ClauseStatus candidate_status(ClauseId id, const Clause& clause, const RowSpace& trail_span,
                                  const SatisfactionRecord& known);

    /// @brief Add `0 = 1` to `intersection_` as its last row.
    void add_false_row_to_intersection();

    /// @brief Put a decision drawn under `CsidsSpan::plus_minus` on the side in the
    /// top clause's `span(neg(C), U)`.
    /// @pre `eqn` lies in the top clause's `span^±(neg(C), U)` and outside `span^±(U)`.
    void orient_to_falsify(Equation& eqn, const Clause& top_clause, const RowSpace& trail_span);

    /// @brief Bump one clause's activity, rescaling everything if it overflows.
    void bump_activity(ClauseId id);

    bool phase_saving() const noexcept { return saved_trails_.keeps_trails(); }
    /// @brief Whether phase saving chooses the next decision's space.
    bool consults_trails() const noexcept {
        return phase_saving() && saved_trails_.consults_trails();
    }

    EquationRepr repr_;
    std::size_t num_vars_;
    Seed base_seed_;
    DecisionPhase decision_phase_;
    mutable Rng phase_rng_;
    DecisionCounters counters_;

    CsidsConfig config_;
    MaxHeap heap_;
    /// @brief The lazy decay: an activity entered or added now is multiplied by this,
    /// which grows by `1 / decay_` per conflict.
    double scale_ = 1.0;
    double decay_; ///< The current mode's.
    std::optional<ClauseId> last_decision_clause_;
    std::optional<double> pending_learned_priority_; ///< Activity awaiting the next new clause.
    ClauseId num_clauses_ = 0;
    ClauseSatisfactionWorkspace clause_sat_workspace_;
    CsidsSpan span_;

    /// @brief Off the heap, by a level found to satisfy them; spans nest, so
    /// they stay satisfied until a backjump below it.
    std::vector<std::vector<ClauseId>> satisfied_by_level_;

    /// @brief The trail's decision level: each equation returned is decided one
    /// level up, and backjumps come through `update_with_backjump()`.
    std::size_t level_ = 0;
    std::vector<ClauseId> set_aside_ids_;
    /// @brief The top clause and the clauses whose intersection shrank the last decision's space.
    std::vector<ClauseId> changed_clause_ids_;

    /// @brief What `decide_from_intersection()` has intersected down to, empty
    /// between decisions, and ending in `0 = 1` under `CsidsSpan::plus_minus`.
    RowSpace intersection_;

    /// @brief Rows `decide_from_intersection()` is done with, which the next
    /// intersection is built in.
    EquationPool intersection_spare_rows_;

    Rng clause_equation_rng_; ///< Stream for sampling a combination of a clause's rows.

    SavedTrails saved_trails_;
};

} // namespace xorcle
