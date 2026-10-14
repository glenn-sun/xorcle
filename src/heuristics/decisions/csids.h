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
#include "util/max_heap.h"
#include "util/random.h"

namespace xorcle {

/// @brief Tunable CSIDS parameters.
struct CsidsConfig {
    /// @brief An asserting clause enters at this multiple of the increment.
    double initial_activity = 1.0;

    double decay = 0.98; ///< `1` never decays, `0` keeps only the newest activity.
    double bump = 1.0; ///< The initial activity increment.

    /// @brief The open clauses, from the top of the heap down, whose linear
    /// negations a decision is drawn from the intersection of; `1` is the top
    /// clause alone.
    std::size_t intersection_depth = 50;

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
    /// @brief The equation-sampling stream derives from `seed`.
    /// @pre `config.decay` satisfies `0 <= decay <= 1` and
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

    /// @brief Put the clauses found satisfied above `level` back in the heap.
    /// @pre `level` is at most the level of the trail's last decision.
    void update_with_backjump(std::size_t level) override;

    /// @brief The replacement inherits the subsumed clause's activity when larger.
    void update_with_subsumed_clause(ClauseId subsumed_id, ClauseId replacement_id) override;

    double clause_activity(ClauseId id) const override;

    std::optional<ClauseId> decision_clause_hint() const override;

    /// @brief The levels whose decision clause still outranks the clause the next
    /// decision would come from.
    std::size_t reusable_levels(
        const RowSpace& trail_span, const std::vector<Clause>& clauses,
        const std::vector<std::uint8_t>& clause_active,
        const std::vector<std::optional<ClauseId>>& decision_clauses,
        const SatisfactionRecord& known = {}) override;

    void update_with_substitution(const IndexSet& dropped_col_idxs,
                                  const std::vector<Clause>& clauses,
                                  const std::vector<std::uint8_t>& clause_active) override;

    void update_with_renumbering(const CompactionTransform& transform) override;

    const IntersectionCounters& intersection_counters() const noexcept {
        return intersection_counters_;
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

    CsidsState state_;
    ClauseSatisfactionWorkspace clause_sat_workspace_;
    std::size_t intersection_depth_;

    /// @brief Off the heap, by a level found to satisfy them; spans nest, so
    /// they stay satisfied until a backjump below it.
    std::vector<std::vector<ClauseId>> satisfied_by_level_;

    /// @brief The trail's decision level: each equation returned is decided one
    /// level up, and backjumps come through `update_with_backjump()`.
    std::size_t level_ = 0;
    IntersectionCounters intersection_counters_;
    std::vector<ClauseId> set_aside_ids_;

    /// @brief What `decide_from_intersection()` has intersected down to; empty
    /// between decisions.
    RowSpace intersection_;

    /// @brief Rows `decide_from_intersection()` is done with, which the next
    /// intersection is built in.
    std::vector<Equation> intersection_spare_rows_;

    Rng clause_equation_rng_; ///< Stream for sampling a combination of a clause's rows.
};

} // namespace xorcle
