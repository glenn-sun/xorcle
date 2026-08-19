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
#include "heuristics/decisions/decision_procedure.h"
#include "heuristics/decisions/decision_utils.h"
#include "util/max_heap.h"
#include "util/random.h"

namespace xorcle {

/// @brief Tunable CSIDS parameters: @f$(A_0, \alpha, \Delta)@f$.
///
/// @warning **Always initialize this by member name.** The members are declared
/// @f$(A_0, \alpha, \Delta)@f$, but a CSIDS triple is conventionally written
/// @f$(A_0, \Delta, \alpha)@f$, so transcribing one positionally silently swaps
/// the bump and the decay. Designated initializers make that impossible; the
/// presets below use them, as should every construction site.
struct CsidsConfig {
    /// @brief @f$A_0@f$: an asserting clause enters at this multiple of the increment.
    double initial_activity = 1.0;

    double decay = 0.98; ///< @f$\alpha@f$: the decay factor. Must satisfy @f$0 < \alpha < 1@f$.
    double bump = 1.0; ///< @f$\Delta@f$: the initial activity increment.
};

/// @brief The CSIDS defaults: @f$A_0 = 1@f$, @f$\Delta = 1@f$, @f$\alpha = 0.98@f$.
inline constexpr CsidsConfig csids_default_config{
    .initial_activity = 1.0, .decay = 0.98, .bump = 1.0};

/// @brief The `berkmin` preset: @f$A_0 = 1@f$, @f$\Delta = 0@f$, @f$\alpha = 0.5@f$.
///
/// @f$\Delta = 0@f$ is what makes it BerkMin-style: a clause's activity is
/// whatever it had on entry and resolution never changes it. @f$\Delta@f$ also
/// seeds the increment, so @f$A_0 \cdot \mathrm{inc}@f$ is zero and the decay
/// has nothing to act on.
inline constexpr CsidsConfig csids_berkmin_config{
    .initial_activity = 1.0, .decay = 0.5, .bump = 0.0};

/// @brief The `cmtf` preset: @f$A_0 = 1@f$, @f$\Delta = 1@f$, @f$\alpha = 0.25@f$.
///
/// A decay below @f$1/2@f$ makes a newly learned clause outrank every older one
/// however often they were bumped: clause move-to-front.
inline constexpr CsidsConfig csids_cmtf_config{
    .initial_activity = 1.0, .decay = 0.25, .bump = 1.0};

struct CsidsParams {
    MaxHeap<ClauseId> activity_heap; ///< Clause ids, keyed by activity.
    double inc_amt = 1.0; ///< What one bump adds. Grows by @f$1/\alpha@f$ per conflict.
    double initial_activity = 1.0; ///< @f$A_0@f$, from the configuration.
    double decay = 0.98; ///< @f$\alpha@f$, from the configuration.

    /// @brief Bump one clause's activity, rescaling everything if it overflows.
    void bump_activity(ClauseId id);
};

struct CsidsState {
    CsidsState(EquationRepr repr, std::size_t num_vars, const CsidsConfig& config);

    /// @brief Rebuild the literal fallback basis if the active width changed.
    void ensure_unit_eqns();
    void reset_activity();

    CsidsParams params;
    std::vector<Equation> unit_eqns; ///< Literal basis used by the fallback path.
    std::optional<ClauseId> last_decision_clause;
    std::optional<double> pending_learned_priority; ///< Activity awaiting the next new clause.
    ClauseId num_clauses = 0;
    double initial_inc_amt = 1.0; ///< @f$\Delta@f$, for restoring `inc_amt` on reset.
    EquationRepr repr;
    std::size_t num_vars;
};

/// @brief Decides from the highest-activity clause the trail has not satisfied.
///
/// Clause activity, not variable activity, is what decays: a clause is bumped
/// each time conflict analysis resolves through it, and a new asserting clause
/// enters at @f$A_0@f$ times the current increment. The decision is a uniformly
/// random non-zero combination of the chosen clause's rows with the configured
/// phase applied. When the heap runs dry the procedure scans the literal basis
/// instead, counted separately as a fallback.
class Csids final : public DecisionProcedure {
public:
    /// @brief The equation-sampling stream derives from `seed`.
    Csids(EquationRepr repr, std::size_t num_vars, Seed seed,
          const CsidsConfig& config = csids_default_config);

    /// @brief Resets both RNG streams as well as activity and the counters.
    void reinitialize() override;

    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;

    /// @brief Record the activity the next new clause will enter at, then decay.
    void update_with_asserting_clause(const Clause& clause) override;

    /// @brief Register a clause id, consuming any pending learned activity.
    void update_with_new_clause(const Clause& clause, ClauseId id) override;

    void update_with_resolved_clause(ClauseId id) override;

    std::optional<ClauseId> decision_clause_hint() const override;

    void update_with_compaction(const CompactionTransform& transform, const RowSpace& trail_span,
                                const std::vector<Clause>& clauses,
                                const std::vector<std::uint8_t>& clause_active) override;

private:
    CsidsState state_;
    ClauseSatisfactionWorkspace clause_sat_workspace_;

    Rng clause_equation_rng_; ///< Stream for sampling a combination of a clause's rows.
    Equation candidate_;
    Equation reduced_candidate_; ///< The partial reduction of `candidate_`.
};

} // namespace xorcle
