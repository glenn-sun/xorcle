#pragma once

/// @file csids.h
/// @brief CSIDS: Clause State Independent Decaying Sum, the default decision procedure.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "heuristics/decisions/clause_activity.h"
#include "heuristics/decisions/clause_decisions.h"
#include "heuristics/decisions/decision_procedure.h"
#include "heuristics/decisions/saved_trails.h"
#include "util/max_heap.h"
#include "util/random.h"

namespace xorcle {

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
    /// @brief An asserting clause enters at this multiple of the increment.
    double initial_activity = 1.0;

    double decay = 0.98; ///< `1` never decays, `0` keeps only the newest activity.
    double focused_decay = 0.5; ///< The decay in focused mode.
    double bump = 1.0; ///< The initial activity increment.

    /// @brief The open clauses, from the top of the heap down, whose linear
    /// negations a decision is drawn from the intersection of; `1` is the top
    /// clause alone.
    std::size_t intersection_depth = 50;

    CsidsSpan span = CsidsSpan::plain;

    PhaseSavingConfig phase_saving;

    bool operator==(const CsidsConfig&) const = default;
};

struct CsidsActivity {
    MaxHeap activity_heap;
    double inc_amt; ///< What one bump adds. Grows by `1 / decay` per conflict.
    double initial_activity;
    double decay;

    /// @brief Bump one clause's activity, rescaling everything if it overflows.
    void bump_activity(ClauseId id);
};

struct CsidsState {
    explicit CsidsState(const CsidsConfig& config);

    void reset_activity();

    CsidsActivity activity;
    std::optional<ClauseId> last_decision_clause;
    std::optional<double> pending_learned_priority; ///< Activity awaiting the next new clause.
    ClauseId num_clauses = 0;
    double initial_inc_amt; ///< For restoring `inc_amt` on reset.
};

/// @brief Decides from the highest-activity clause the trail has not satisfied.
///
/// Activity is per clause: bumped each time conflict analysis resolves through
/// it, decayed at every conflict. See arXiv:2605.15002 for details.
class Csids final : public DecisionProcedure, public ClauseActivitySource {
public:
    /// @brief The equation-sampling stream derives from `seed`; the mode is stable.
    /// @pre `config.decay` and `config.focused_decay` lie in `[0, 1]`, and
    /// `config.initial_activity` and `config.bump` are non-negative.
    Csids(EquationRepr repr, std::size_t num_vars, Seed seed,
          const CsidsConfig& config = {});

    /// @brief Resets both RNG streams as well as activity and the counters.
    void reinitialize() override;

    DecisionResult make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses,
                                 const std::vector<std::uint8_t>& clause_active,
                                 const SatisfactionRecord& known = {}) override;

    /// @brief Record the activity the next new clause will enter at, then decay.
    void update_with_asserting_clause(const Clause& clause) override;

    /// @brief Register a clause id, consuming any pending learned activity.
    void update_with_new_clause(const Clause& clause, ClauseId id) override;

    void update_with_resolved_clause(ClauseId id) override;

    /// @brief Save the conflict's trail for phase saving.
    void update_with_conflict(const Trail& trail, std::span<const std::size_t> level_stamps) override;

    /// @brief Put the clauses found satisfied above `level` back in the heap.
    /// @pre `level` is at most the level of the trail's last decision.
    void update_with_backjump(std::size_t level) override;

    void update_with_replayed_decision() override { ++level_; }

    /// @brief Decay by the mode's decay, and consult the target trail only in stable mode.
    void update_with_mode(SearchMode mode) override;

    /// @brief The replacement inherits the subsumed clause's activity when larger.
    void update_with_subsumed_clause(ClauseId subsumed_id, ClauseId replacement_id) override;

    /// @brief Put the clause back in the heap.
    void update_with_narrowed_clause(ClauseId id) override;

    double clause_activity(ClauseId id) const override;

    std::optional<ClauseId> decision_clause_hint() const override;

    void update_with_substitution(const IndexSet& dropped_col_idxs,
                                  const std::vector<Clause>& clauses,
                                  const std::vector<std::uint8_t>& clause_active) override;

    void update_with_renumbering(const CompactionTransform& transform) override;

    const IntersectionCounters& intersection_counters() const noexcept {
        return intersection_counters_;
    }

    const PhaseSavingCounters& phase_saving_counters() const noexcept {
        return saved_trails_.counters();
    }

    void add_to_stats(RunStats& stats) const override;

private:
    /// @brief Intersect the open top clause's linear negation, modulo the trail,
    /// greedily with those of the open clauses below it, keeping each
    /// intersection that stays outside the trail span.
    /// @return A decision from the intersection, a clause below the top that the
    /// trail leaves propagating, or nothing when no intersection was kept.
    /// @pre `top_id` is the heap's maximum, the trail leaves it open, and it
    /// was the last clause checked.
    std::optional<DecisionResult> decide_from_intersection(
        ClauseId top_id, const RowSpace& trail_span, const std::vector<Clause>& clauses,
        const std::vector<std::uint8_t>& clause_active, const SatisfactionRecord& known);

    /// @brief What the trail leaves of the heap's top clause; a satisfied one
    /// comes off the heap until a backjump discards the level found to satisfy it.
    /// @pre `id` is the heap's maximum and names `clause`.
    ClauseStatus candidate_status(ClauseId id, const Clause& clause, const RowSpace& trail_span,
                                  const SatisfactionRecord& known);

    /// @brief Move every row of `intersection_` to `intersection_spare_rows_`.
    void clear_intersection();

    /// @brief Add `0 = 1` to `intersection_` as its last row.
    void add_false_row_to_intersection();

    /// @brief Put a decision drawn under `CsidsSpan::plus_minus` on the side in the
    /// top clause's `span(neg(C), U)`.
    /// @pre `eqn` lies in the top clause's `span^±(neg(C), U)` and outside `span^±(U)`.
    void orient_to_falsify(Equation& eqn, const Clause& top_clause, const RowSpace& trail_span);

    bool phase_saving() const noexcept { return saves_trails(decision_phase()); }
    /// @brief Whether phase saving chooses the next decision's space.
    bool consults_trails() const noexcept {
        return phase_saving() && saved_trails_.consults_trails();
    }

    CsidsState state_;
    double stable_decay_;
    double focused_decay_;
    ClauseSatisfactionWorkspace clause_sat_workspace_;
    std::size_t intersection_depth_;
    CsidsSpan span_;

    /// @brief Off the heap, by a level found to satisfy them; spans nest, so
    /// they stay satisfied until a backjump below it.
    std::vector<std::vector<ClauseId>> satisfied_by_level_;

    /// @brief The trail's decision level: each equation returned is decided one
    /// level up, and backjumps come through `update_with_backjump()`.
    std::size_t level_ = 0;
    IntersectionCounters intersection_counters_;
    std::vector<ClauseId> set_aside_ids_;
    /// @brief The top clause and the clauses whose intersection shrank the last decision's space.
    std::vector<ClauseId> changed_clause_ids_;

    /// @brief What `decide_from_intersection()` has intersected down to, empty
    /// between decisions, and ending in `0 = 1` under `CsidsSpan::plus_minus`.
    RowSpace intersection_;

    /// @brief Rows `decide_from_intersection()` is done with, which the next
    /// intersection is built in.
    std::vector<Equation> intersection_spare_rows_;

    Rng clause_equation_rng_; ///< Stream for sampling a combination of a clause's rows.

    SavedTrails saved_trails_;
};

} // namespace xorcle
