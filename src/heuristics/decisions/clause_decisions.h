#pragma once

/// @file clause_decisions.h
/// @brief Shared machinery the decision procedures build on.

#include <cstddef>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "util/random.h"

namespace xorcle {

class ClauseSatisfactionWorkspace {
public:
    ClauseSatisfactionWorkspace(EquationRepr repr, std::size_t num_vars);

    void reconfigure(std::size_t num_vars);

private:
    friend bool clause_satisfied_by_trail(const RowSpace& trail_span, const Clause& clause,
                                          ClauseSatisfactionWorkspace& workspace);

    void clear_local_basis();

    EquationRepr repr_;
    std::size_t num_vars_;

    Equation false_equation_;

    /// @brief `0 = 1` reduced by the trail span, then by each row added to the local basis.
    Equation scratch_false_;

    Equation scratch_row_;
    RowSpace local_basis_;
};

/// @brief Choose the first basis equation, in order, the trail has not already decided.
/// @return A reference into `basis`, which is *not* reduced.
/// @pre `basis` is non-empty, every equation in it has `trail_span.cols()` bits and
/// `trail_span`'s representation, and some legal decision remains.
const Equation& decide_from_basis(const std::vector<Equation>& basis, const RowSpace& trail_span);

/// @brief Whether the trail already satisfies a clause, under every extension.
/// @pre `clause.cols() == trail_span.cols()`, and `workspace` was configured for
/// `trail_span.cols() - 1` variables in `trail_span`'s representation.
bool clause_satisfied_by_trail(const RowSpace& trail_span, const Clause& clause,
                               ClauseSatisfactionWorkspace& workspace);

/// @brief Compact a literal basis, preserving basis order.
/// @return The first compacted equation for each new leading bit, in old basis order, then
/// a unit equation for each new variable none covered.
/// @pre Every equation in `basis` has `transform.old_num_vars() + 1` bits and the
/// representation `repr` names.
/// @post Exactly `transform.new_num_vars()` equations, one per leading bit.
std::vector<Equation> compact_basis_keep_order(const std::vector<Equation>& basis,
                                               const CompactionTransform& transform,
                                               EquationRepr repr);

struct SampledDecision {
    Equation candidate; ///< The combination of clause rows drawn, unreduced.
    Equation reduced; ///< `candidate` partially reduced by the trail span.
};

/// @brief Sample a random combination of a clause's rows that is a legal decision, by
/// rejection sampling as in arXiv:2605.15002.
/// @pre `clause.cols() == trail_span.cols()`, `clause.repr() == trail_span.repr()`,
/// `clause.rows() > 0`, and the trail does not satisfy the clause.
SampledDecision sample_equation_from_clause(const Clause& clause, const RowSpace& trail_span,
                                            Rng& rng);

} // namespace xorcle
