#pragma once

/// @file decision_utils.h
/// @brief Shared machinery the decision procedures build on: the clause
/// satisfaction test, basis scanning and projection, and clause sampling.

#include <cstddef>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "util/random.h"

namespace xorcle {

/// @brief Reusable scratch for `clause_satisfied_by_trail()`. The small echelon
/// basis and the two equations fed through it are the whole cost of that test,
/// so they are refilled here rather than reallocated per call.
class ClauseSatisfactionWorkspace {
public:
    ClauseSatisfactionWorkspace(EquationRepr repr, std::size_t num_vars);

    void reconfigure(std::size_t num_vars);

    std::size_t num_vars() const noexcept { return num_vars_; }

private:
    friend bool clause_satisfied_by_trail(const RowSpace& trail_span, const Clause& clause,
                                          ClauseSatisfactionWorkspace& workspace);

    void clear_local_basis(); ///< Empties it, keeping the storage it has grown into.

    EquationRepr repr_;
    std::size_t num_vars_;

    Equation false_equation_; ///< A preallocated @f$\mathbf{1}@f$; a constant, not scratch.

    /// @brief @f$\mathbf{1}@f$ reduced by the trail span, then incrementally by
    /// each clause row added to the local basis.
    Equation scratch_false_;

    Equation scratch_row_; ///< One clause row, reduced by the trail span.
    RowSpace local_basis_; ///< The clause's reduced rows, in echelon form.
};

/// @brief Choose the first basis equation, in order, the trail has not already
/// decided; `reduced_scratch` takes each reduction in turn.
/// @return The first candidate whose reduction has a leading bit, i.e. is a
/// legal decision. It points into `basis` and is *not* reduced.
/// @pre Both have width `trail_span.cols()`, and some legal decision remains.
const Equation& decide_from_basis(const std::vector<Equation>& basis, const RowSpace& trail_span,
                                  Equation& reduced_scratch);

/// @brief Whether the trail already satisfies a clause, under every extension.
/// A clause @f$\bigwedge_i g_i@f$ is *falsified* exactly when every @f$g_i \in
/// \mathrm{span}(U)@f$, and *satisfied* exactly when @f$\mathbf{1} \in
/// \mathrm{span}(U) + \langle g_1, \dots, g_k \rangle@f$; the second is computed
/// incrementally, stopping as soon as it holds.
/// @pre `workspace.num_vars() + 1 == trail_span.cols()`.
bool clause_satisfied_by_trail(const RowSpace& trail_span, const Clause& clause,
                               ClauseSatisfactionWorkspace& workspace);

/// @brief Project a literal basis through a compaction, preserving basis order.
/// @return The first projected equation for each new leading bit, in old basis
/// order, then a unit equation for each new variable none covered.
/// @pre Every equation in `basis` has width `transform.old_num_vars() + 1`.
/// @post Exactly `transform.new_num_vars()` equations, one per leading bit.
std::vector<Equation> project_basis_keep_order(const std::vector<Equation>& basis,
                                               const CompactionTransform& transform,
                                               EquationRepr repr);

/// @brief Sample a random combination of a clause's rows that is a legal
/// decision, into `out_candidate` unreduced and `out_reduced` partially reduced.
/// @pre `clause.rows() > 0`; the out-params have width `trail_span.cols()`; and
/// the clause is not satisfied by the trail, i.e. `clause_satisfied_by_trail()`
/// returned false for it.
///
/// Rejection sampling, so the draw count is data-dependent;
/// `random_nonzero_coefficients()` documents the per-attempt pattern. Only a
/// reduction with a leading bit is accepted: @f$\mathbf{0}@f$ is no decision,
/// @f$\mathbf{1}@f$ would conflict at once, and both are resampled. The
/// @f$\mathbf{1}@f$ branch is unreachable from the intended caller — it means
/// @f$\mathbf{1} \in \mathrm{span}(U) + \langle g_i \rangle@f$, exactly what the
/// satisfaction precondition rules out; that precondition is also what makes the
/// loop terminate, and debug builds check it.
bool sample_equation_from_clause(const Clause& clause, const RowSpace& trail_span, Rng& rng,
                                 Equation& out_candidate, Equation& out_reduced);

} // namespace xorcle
