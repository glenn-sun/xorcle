#pragma once

/// @file fixed_literals.h
/// @brief The fixed-literal decision procedure.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "heuristics/decisions/decision_procedure.h"
#include "util/random.h"

namespace xorcle {

/// @brief Decides the first variable the trail has not yet determined.
///
/// The literal basis is scanned from the front on every decision, and the phase
/// is not applied, so decisions depend on neither clause activity nor the seed.
class FixedLiterals final : public DecisionProcedure {
public:
    FixedLiterals(EquationRepr repr, std::size_t num_vars, Seed seed);

    void reinitialize() override;

    DecisionResult make_decision(const RowSpace& trail_span, const std::vector<Clause>& clauses,
                                 const std::vector<std::uint8_t>& clause_active,
                                 const SatisfactionRecord& known = {}) override;

    void update_with_substitution(const IndexSet& dropped_col_idxs,
                                  const std::vector<Clause>& clauses,
                                  const std::vector<std::uint8_t>& clause_active) override;

    void update_with_renumbering(const CompactionTransform& transform) override;

private:
    std::vector<Equation> decision_basis_; ///< In variable order, without the dropped ones.
};

} // namespace xorcle
