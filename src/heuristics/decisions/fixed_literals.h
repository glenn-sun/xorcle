#pragma once

/// @file fixed_literals.h
/// @brief The fixed-literal decision procedure: a debug baseline.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "algebra/algebra.h"
#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "heuristics/decisions/decision_procedure.h"
#include "util/random.h"

namespace xorcle {

/// @brief Decides the first variable the trail has not yet determined.
///
/// The basis is @f$x_0, x_1, \dots, x_{n-1}@f$ in variable order, scanned from
/// the front on every decision. No activity, no randomness, no clause
/// involvement, so a run is a deterministic function of the input alone.
///
/// @warning **This is a debug baseline, not a serious heuristic.** It is kept
/// only because a decision order that never changes makes divergences between
/// builds, or between the algebra backends, instantly attributable.
class FixedLiterals final : public DecisionProcedure {
public:
    FixedLiterals(Algebra backend, std::size_t num_vars, Seed seed);

    void reinitialize() override;

    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;

    /// @brief Project the literal basis into the compacted variable space.
    void update_with_compaction(const CompactionTransform& transform, const RowSpace& trail_span,
                                const std::vector<Clause>& clauses,
                                const std::vector<std::uint8_t>& clause_active) override;

private:
    std::vector<Equation> decision_basis_; ///< The literal basis, in variable order.
    Equation reduced_scratch_; ///< The reduction of the candidate under test.
};

} // namespace xorcle
