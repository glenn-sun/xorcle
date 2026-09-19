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
};

inline constexpr CsidsConfig csids_default_config{
    .initial_activity = 1.0, .decay = 0.98, .bump = 1.0};

/// @brief `bump = 0` leaves every clause at activity zero, never bumped.
inline constexpr CsidsConfig csids_berkmin_config{
    .initial_activity = 1.0, .decay = 0.5, .bump = 0.0};

/// @brief A decay below `1/2` makes a newly learned clause outrank every older
/// one however often they were bumped: clause move-to-front.
inline constexpr CsidsConfig csids_cmtf_config{
    .initial_activity = 1.0, .decay = 0.25, .bump = 1.0};

struct CsidsActivity {
    MaxHeap<ClauseId> activity_heap;
    double inc_amt = 1.0; ///< What one bump adds. Grows by `1 / decay` per conflict.
    double initial_activity = 1.0;
    double decay = 0.98;

    /// @brief Bump one clause's activity, rescaling everything if it overflows.
    void bump_activity(ClauseId id);
};

struct CsidsState {
    CsidsState(EquationRepr repr, std::size_t num_vars, const CsidsConfig& config);

    /// @brief Rebuild the literal fallback basis if the active variable count changed.
    void ensure_unit_eqns();
    void reset_activity();

    CsidsActivity activity;
    std::vector<Equation> unit_eqns; ///< Literal basis used by the fallback path.
    std::optional<ClauseId> last_decision_clause;
    std::optional<double> pending_learned_priority; ///< Activity awaiting the next new clause.
    ClauseId num_clauses = 0;
    double initial_inc_amt = 1.0; ///< For restoring `inc_amt` on reset.
    EquationRepr repr;
    std::size_t num_vars;
};

/// @brief Decides from the highest-activity clause the trail has not satisfied.
///
/// Activity is per clause: bumped each time conflict analysis resolves through
/// it, decayed at every conflict. See arXiv:2605.15002 for details. When the
/// heap runs dry the procedure scans the literal basis instead, counted
/// separately as a fallback.
class Csids final : public DecisionProcedure, public ClauseActivitySource {
public:
    /// @brief The equation-sampling stream derives from `seed`.
    /// @pre `config.decay` satisfies `0 <= decay <= 1` and
    /// `config.initial_activity` and `config.bump` are non-negative.
    Csids(EquationRepr repr, std::size_t num_vars, Seed seed,
          const CsidsConfig& config = csids_default_config);

    /// @brief Resets both RNG streams as well as activity and the counters.
    void reinitialize() override;

    DecisionResult make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses,
                                 const std::vector<std::uint8_t>& clause_active) override;

    /// @brief Record the activity the next new clause will enter at, then decay.
    void update_with_asserting_clause(const Clause& clause) override;

    /// @brief Register a clause id, consuming any pending learned activity.
    void update_with_new_clause(const Clause& clause, ClauseId id) override;

    void update_with_resolved_clause(ClauseId id) override;

    /// @brief Put the clauses the discarded level satisfied back in the heap.
    void update_with_undone_decision() override;

    /// @brief The replacement inherits the subsumed clause's activity when larger.
    void update_with_subsumed_clause(ClauseId subsumed_id, ClauseId replacement_id) override;

    double clause_activity(ClauseId id) const override;

    std::optional<ClauseId> decision_clause_hint() const override;

    /// @brief The levels whose decision clause still outranks the clause the next
    /// decision would come from.
    std::size_t reusable_levels(
        const RowSpace& trail_span, const std::vector<Clause>& clauses,
        const std::vector<std::uint8_t>& clause_active,
        const std::vector<std::optional<ClauseId>>& decision_clauses) override;

    void update_with_compaction(const CompactionTransform& transform, const RowSpace& trail_span,
                                const std::vector<Clause>& clauses,
                                const std::vector<std::uint8_t>& clause_active) override;

private:
    CsidsState state_;
    ClauseSatisfactionWorkspace clause_sat_workspace_;

    Rng clause_equation_rng_; ///< Stream for sampling a combination of a clause's rows.
};

} // namespace xorcle
