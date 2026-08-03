#pragma once

/// @file csids.h
/// @brief CSIDS: Clause State Independent Decaying Sum, the default decision
/// procedure.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "algebra/algebra.h"
#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "heuristics/decisions/decision_procedure.h"
#include "heuristics/decisions/decision_utils.h"
#include "util/max_heap.h"
#include "util/random.h"

namespace xorcle {

/// @brief Tunable CSIDS parameters.
///
/// The paper's three quantities, under the names the paper gives them:
/// @f$A_0@f$ is `initial_activity`, @f$\alpha@f$ is `decay`, @f$\Delta@f$ is
/// `bump`.
///
/// @warning **Always initialize this by member name.** The declaration order is
/// @f$(A_0, \alpha, \Delta)@f$, inherited from the 0.1 tree's `CVsidsConfig` and
/// from the 0.1 command line's `-d clause_vsids <base> <decay> <bump>`. The
/// paper writes its preset tuples
/// as @f$(A_0, \Delta, \alpha)@f$, so transcribing one of those tuples
/// positionally silently swaps the bump and the decay. Designated initializers
/// make that impossible; the presets below use them, and so should every other
/// construction site.
struct CsidsConfig {
    /// @brief @f$A_0@f$: a new asserting clause starts at this multiple of the
    /// current increment.
    double initial_activity = 1.0;

    /// @brief @f$\alpha@f$: the decay factor. Must satisfy @f$0 < \alpha < 1@f$.
    double decay = 0.98;

    /// @brief @f$\Delta@f$: the initial activity increment.
    double bump = 1.0;
};

/// @brief The CSIDS defaults fixed by the paper: @f$A_0 = 1@f$, @f$\Delta = 1@f$,
/// @f$\alpha = 0.98@f$.
inline constexpr CsidsConfig csids_default_config{
    .initial_activity = 1.0, .decay = 0.98, .bump = 1.0};

/// @brief The `berkmin` preset: @f$A_0 = 1@f$, @f$\Delta = 0@f$, @f$\alpha = 0.5@f$.
///
/// The paper allows any @f$0 < \alpha < 1@f$ here; @f$\Delta = 0@f$ is what
/// makes it BerkMin-style, because with no bump a clause's activity is whatever
/// it was given when it entered and resolution never changes it.
///
/// @note @f$\Delta@f$ also seeds the increment, so @f$\Delta = 0@f$ means both
/// the resolution bump and @f$A_0 \cdot \mathrm{inc}@f$ are zero, and the decay
/// has nothing to act on. The values are the 0.1 tree's, carried across
/// unchanged.
inline constexpr CsidsConfig csids_berkmin_config{
    .initial_activity = 1.0, .decay = 0.5, .bump = 0.0};

/// @brief The `cmtf` preset: @f$A_0 = 1@f$, @f$\Delta = 1@f$, @f$\alpha = 0.25@f$.
///
/// A decay below @f$1/2@f$ makes a newly learned clause outrank every older one
/// however often the older ones were bumped, giving clause move-to-front
/// behaviour. The paper allows any @f$0 < \alpha < 0.5@f$.
inline constexpr CsidsConfig csids_cmtf_config{
    .initial_activity = 1.0, .decay = 0.25, .bump = 1.0};

/// @brief Clause-activity bookkeeping for CSIDS.
struct CsidsParams {
    /// @brief Heap of clause ids, keyed by activity.
    MaxHeap<ClauseId> activity_heap;

    /// @brief The amount a single bump adds. Grows by @f$1/\alpha@f$ per conflict.
    double inc_amt = 1.0;

    /// @brief @f$A_0@f$, from the configuration.
    double initial_activity = 1.0;

    /// @brief @f$\alpha@f$, from the configuration.
    double decay = 0.98;

    /// @brief Bump one clause's activity, rescaling everything if it overflows.
    /// @param id The clause to bump.
    void bump_activity(ClauseId id);
};

/// @brief The activity state CSIDS carries across a run.
struct CsidsState {
    /// @brief Construct the state for a given active variable count.
    /// @param backend Storage representation to use.
    /// @param num_vars Number of active variable columns.
    /// @param config Tunable parameters.
    CsidsState(Algebra backend, std::size_t num_vars, const CsidsConfig& config);

    /// @brief Rebuild the literal fallback basis if the active width changed.
    void ensure_unit_eqns();

    /// @brief Reset clause activity for a fresh run.
    void reset_activity();

    CsidsParams params; ///< Activity parameters and heap.
    std::vector<Equation> unit_eqns; ///< Literal basis used by the fallback path.
    std::optional<ClauseId> last_decision_clause; ///< Clause the last decision came from.
    std::optional<double> pending_learned_priority; ///< Activity awaiting the next new clause.
    ClauseId num_clauses = 0; ///< Number of clause ids registered so far.
    double initial_inc_amt = 1.0; ///< @f$\Delta@f$, for restoring `inc_amt` on reset.
    Algebra backend; ///< Storage representation for the literal basis.
    std::size_t num_vars; ///< Number of active variable columns.
};

/// @brief Decides from the highest-activity clause the trail has not satisfied.
///
/// Clause activity, not variable activity, is what decays: a clause is bumped
/// each time it is resolved through during conflict analysis, and a newly
/// learned asserting clause enters at @f$A_0@f$ times the current increment. The
/// decision itself is a uniformly random non-zero combination of the chosen
/// clause's rows, with the configured phase applied.
///
/// When the heap runs dry the procedure falls back to scanning the literal
/// basis, and those decisions are counted separately as fallbacks.
class Csids final : public DecisionProcedure {
public:
    /// @brief Construct a CSIDS procedure.
    /// @param backend Storage representation to use.
    /// @param num_vars Number of active variable columns.
    /// @param seed Base solver seed; the equation-sampling stream derives from it.
    /// @param config Tunable parameters; defaults to `csids_default_config`.
    Csids(Algebra backend, std::size_t num_vars, Seed seed,
          const CsidsConfig& config = csids_default_config);

    /// @brief Reset activity, both RNG streams, and the counters for a fresh run.
    void reinitialize() override;

    /// @brief Sample a decision from the highest-activity unsatisfied clause.
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;

    /// @brief Record the activity the next new clause will enter at, then decay.
    void update_with_asserting_clause(const Clause& clause) override;

    /// @brief Register a clause id, consuming any pending learned activity.
    void update_with_new_clause(const Clause& clause, ClauseId id) override;

    /// @brief Bump a clause used as a reason during conflict analysis.
    void update_with_resolved_clause(ClauseId id) override;

    /// @brief The clause the last decision was sampled from.
    std::optional<ClauseId> decision_clause_hint() const override;

    /// @brief Adopt the compacted variable space and clause count.
    void update_with_compaction(const CompactionTransform& transform, const RowSpace& trail_span,
                                const std::vector<Clause>& clauses,
                                const std::vector<std::uint8_t>& clause_active) override;

private:
    CsidsState state_;

    /// @brief Reusable scratch for the clause-satisfaction test.
    ClauseSatisfactionWorkspace clause_sat_workspace_;

    /// @brief RNG stream for sampling a combination of a clause's rows.
    Rng clause_equation_rng_;

    /// @brief Preallocated scratch for the sampled combination and its reduction.
    ///
    /// The 0.1 tree constructed both per call to `make_decision()`, i.e. once
    /// per decision.
    Equation candidate_;
    Equation reduced_candidate_; ///< The partial reduction of `candidate_`.
};

} // namespace xorcle
