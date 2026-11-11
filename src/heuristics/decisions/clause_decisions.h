#pragma once

/// @file clause_decisions.h
/// @brief What a trail leaves of a clause, and drawing a decision from one.

#include <cstddef>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_pool.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"

namespace xorcle {

/// @brief What a trail leaves of a clause: one every extension satisfies, one of
/// dimension two or more, or one of dimension one or zero, which implies an
/// equation or a contradiction rather than offering a decision.
enum class ClauseStatus { satisfied, open, propagating };

class ClauseSatisfactionWorkspace {
public:
    ClauseSatisfactionWorkspace(EquationRepr repr, std::size_t num_vars);

    void reconfigure(std::size_t num_vars);

    /// @brief After a check that found its clause open or propagating, that
    /// clause's linear negation reduced by the trail span.
    const RowSpace& reduced_clause() const noexcept { return local_basis_; }

private:
    friend ClauseStatus clause_status_by_trail(const RowSpace& trail_span, const Clause& clause,
                                               ClauseSatisfactionWorkspace& workspace,
                                               std::vector<Equation>* reduced_rows);

    EquationRepr repr_;
    std::size_t num_vars_;

    RowSpace local_basis_;

    /// @brief Rows popped from the local basis, reused so a check allocates nothing.
    EquationPool spare_rows_;
};

/// @brief What the trail leaves of a clause, under every extension.
/// @param reduced_rows When non-null and the clause is not satisfied, receives
/// each clause row reduced by `trail_span`, in row order. Compaction passes it
/// once per rewritten clause, a median of 1000 per compaction.
/// @pre `clause.cols() == trail_span.cols()`, and `workspace` was configured for
/// `trail_span.cols() - 1` variables in `trail_span`'s representation.
ClauseStatus clause_status_by_trail(const RowSpace& trail_span, const Clause& clause,
                                    ClauseSatisfactionWorkspace& workspace,
                                    std::vector<Equation>* reduced_rows = nullptr);

} // namespace xorcle
