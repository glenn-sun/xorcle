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
/// the front on every decision. There is no activity, no randomness and no
/// clause involvement, so a run is a deterministic function of the input alone.
///
/// @warning **This is a debug baseline, not a solver strategy.** It does not
/// appear in the paper and is not competitive; it is ported and kept only
/// because a decision order that never changes makes divergences between two
/// builds, or between the two algebra backends, immediately attributable.
class FixedLiterals final : public DecisionProcedure {
public:
    /// @brief Construct a fixed-literal procedure.
    /// @param backend Storage representation to use.
    /// @param num_vars Number of active variable columns.
    /// @param seed Base solver seed.
    FixedLiterals(Algebra backend, std::size_t num_vars, Seed seed);

    /// @brief Rebuild the literal basis for a fresh run.
    void reinitialize() override;

    /// @brief Return the first basis literal the trail has not determined.
    Equation make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses) override;

    /// @brief Project the literal basis into the compacted variable space.
    void update_with_compaction(const CompactionTransform& transform, const RowSpace& trail_span,
                                const std::vector<Clause>& clauses,
                                const std::vector<std::uint8_t>& clause_active) override;

private:
    /// @brief The literal basis, in variable order.
    std::vector<Equation> decision_basis_;

    /// @brief Preallocated scratch holding the reduction of the candidate under test.
    Equation reduced_scratch_;
};

} // namespace xorcle
