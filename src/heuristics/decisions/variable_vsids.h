#pragma once

/// @file variable_vsids.h
/// @brief VSIDS over per-variable literal equations.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "algebra/algebra.h"
#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "heuristics/decisions/decision_procedure.h"
#include "util/max_heap.h"
#include "util/random.h"

namespace xorcle {

/// @brief Activity bookkeeping for the variable-VSIDS family.
struct VsidsParams {
    /// @brief Heap of literal equations, keyed by activity.
    MaxHeap<Equation, EquationHash> activity_heap;

    /// @brief The amount a single bump adds. Grows by @f$1/\alpha@f$ per conflict.
    double inc_amt = 1.0;

    /// @brief @f$\alpha@f$, the decay factor. Must satisfy @f$0 < \alpha < 1@f$.
    double decay = 0.98;

    /// @brief Bump one equation's activity, rescaling everything if it overflows.
    /// @param eqn The literal equation to bump.
    /// @note Rather than decaying every score per conflict, the increment grows;
    /// rescaling by @f$10^{-100}@f$ once a score passes @f$10^{100}@f$ keeps the
    /// two equivalent while staying in range.
    void bump_activity(const Equation& eqn);
};

/// @brief Decides the highest-activity variable the trail has not determined.
///
/// One heap element per variable, holding the unit equation @f$x_v@f$. Activity
/// is bumped for the leading variable of every equation resolved through during
/// conflict analysis and of every row of the resulting asserting clause.
///
/// @note `make_decision()` returns the *reduced* candidate, and the rejected
/// candidates stay extracted from the heap until the next
/// `update_with_asserting_clause()` rebuilds it. Both are load-bearing for
/// reproducing the 0.1 tree's search order.
class VariableVsids final : public DecisionProcedure {
public:
    /// @brief Construct a variable-VSIDS procedure.
    /// @param backend Storage representation to use.
    /// @param num_vars Number of active variable columns.
    /// @param seed Base solver seed.
    VariableVsids(Algebra backend, std::size_t num_vars, Seed seed);

    /// @brief Reset activity, the phase RNG, and the counters for a fresh run.
    void reinitialize() override;

    /// @brief Return the highest-activity literal that is still a legal decision.
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;

    /// @brief Bump the leading variable of an equation resolved through.
    void update_with_critical_equation(const Equation& eqn) override;

    /// @brief Bump every row of the asserting clause, then decay and rebuild.
    void update_with_asserting_clause(const Clause& clause) override;

    /// @brief Project the literal basis and remap every heap key.
    void update_with_compaction(const CompactionTransform& transform, const RowSpace& trail_span,
                                const std::vector<Clause>& clauses,
                                const std::vector<std::uint8_t>& clause_active) override;

private:
    /// @brief Rebuild the heap from scratch for a fresh run.
    void reset_decision_heap_for_fresh_run();

    /// @brief Restore an unexpectedly empty heap mid-solve.
    void refill_decision_heap();

    VsidsParams vsids_params_;

    /// @brief The unit equation for each active variable.
    std::vector<Equation> unit_eqns_;
};

} // namespace xorcle
