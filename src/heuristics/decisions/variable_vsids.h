#pragma once

/// @file variable_vsids.h
/// @brief VSIDS over per-variable literal equations.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "heuristics/decisions/decision_procedure.h"
#include "util/max_heap.h"
#include "util/random.h"

namespace xorcle {

struct VsidsParams {
    MaxHeap<Equation, EquationHash> activity_heap;
    double inc_amt = 1.0; ///< What one bump adds. Grows by `1 / decay` per conflict.
    double decay = 0.98; ///< Must satisfy `0 < decay < 1`.

    /// @brief Bump one equation's activity, rescaling everything if it overflows.
    void bump_activity(const Equation& eqn);
};

/// @brief Decides the highest-activity variable the trail has not determined.
///
/// One heap element per variable. Activity is bumped for the leading variable of each
/// equation conflict analysis resolves through, and of each asserting clause row.
///
/// @note Rejected candidates stay extracted from the heap until the next
/// `update_with_asserting_clause()` rebuilds it, which bears on the search order.
class VariableVsids final : public DecisionProcedure {
public:
    VariableVsids(EquationRepr repr, std::size_t num_vars, Seed seed);

    void reinitialize() override;

    std::optional<Equation> make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses,
                           const std::vector<std::uint8_t>& clause_active) override;

    /// @brief Bump the leading variable of an equation resolved through.
    void update_with_critical_equation(const Equation& eqn) override;

    /// @brief Bump every row of the asserting clause, then decay and rebuild.
    void update_with_asserting_clause(const Clause& clause) override;

    void update_with_compaction(const CompactionTransform& transform, const RowSpace& trail_span,
                                const std::vector<Clause>& clauses,
                                const std::vector<std::uint8_t>& clause_active) override;

private:
    void reset_decision_heap_for_fresh_run();

    /// @brief Restore an unexpectedly empty heap mid-solve.
    void refill_decision_heap();

    VsidsParams vsids_params_;
    std::vector<Equation> unit_eqns_;
};

} // namespace xorcle
