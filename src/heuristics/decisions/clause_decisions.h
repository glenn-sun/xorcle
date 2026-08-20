#pragma once

/// @file clause_decisions.h
/// @brief Shared machinery the decision procedures build on: the clause
/// satisfaction test, basis scanning and compaction, and clause sampling.

#include <cstddef>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "util/random.h"

namespace xorcle {

/// @brief In-out scratch for `clause_satisfied_by_trail()`, not an out-parameter:
/// the test reads and writes this object across calls and its contents are never
/// a result the caller consumes. The small echelon basis and the two equations
/// fed through it are the whole cost of the test, and `Csids::make_decision()`
/// carries one instance as a member -- on `random_kxnf_n_n18_k4_s5` the test runs
/// 5,102 times over 1,436 decisions -- so the basis keeps the storage it has
/// grown into instead of being rebuilt per probe.
class ClauseSatisfactionWorkspace {
public:
    ClauseSatisfactionWorkspace(EquationRepr repr, std::size_t num_vars);

    void reconfigure(std::size_t num_vars);

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
/// decided.
/// @return The first candidate whose reduction has a leading bit, i.e. is a
/// legal decision. It points into `basis` and is *not* reduced.
/// @pre `basis` is non-empty, every equation in it has `trail_span.cols()` bits
/// and `trail_span`'s representation, and some legal decision remains.
const Equation& decide_from_basis(const std::vector<Equation>& basis, const RowSpace& trail_span);

/// @brief Whether the trail already satisfies a clause, under every extension.
/// A clause @f$\bigwedge_i g_i@f$ is *falsified* exactly when every @f$g_i \in
/// \mathrm{span}(U)@f$, and *satisfied* exactly when @f$\mathbf{1} \in
/// \mathrm{span}(U) + \langle g_1, \dots, g_k \rangle@f$; the second is computed
/// incrementally, stopping as soon as it holds.
/// @pre `clause.cols() == trail_span.cols()`, and `workspace` was configured for
/// `trail_span.cols() - 1` variables in `trail_span`'s representation.
bool clause_satisfied_by_trail(const RowSpace& trail_span, const Clause& clause,
                               ClauseSatisfactionWorkspace& workspace);

/// @brief Compact a literal basis, preserving basis order.
/// @return The first compacted equation for each new leading bit, in old basis
/// order, then a unit equation for each new variable none covered.
/// @pre Every equation in `basis` has `transform.old_num_vars() + 1` bits and
/// the representation `repr` names; the result mixes compacted basis equations
/// with fresh ones built in `repr`, so a mismatch yields a basis of two
/// representations that only fails much later.
/// @post Exactly `transform.new_num_vars()` equations, one per leading bit.
std::vector<Equation> compact_basis_keep_order(const std::vector<Equation>& basis,
                                               const CompactionTransform& transform,
                                               EquationRepr repr);

/// @brief One sampled decision.
struct SampledDecision {
    Equation candidate; ///< The combination of clause rows drawn, unreduced.
    Equation reduced; ///< `candidate` partially reduced by the trail span.
};

/// @brief Sample a random combination of a clause's rows that is a legal decision.
/// @pre `clause.cols() == trail_span.cols()`, `clause.repr() == trail_span.repr()`
/// and `clause.rows() > 0`.
/// @pre The clause is not satisfied by the trail, i.e.
/// `clause_satisfied_by_trail()` returned false for it.
///
/// Rejection sampling, so the draw count is data-dependent;
/// `random_nonzero_coefficients()` documents the per-attempt pattern. Only a
/// reduction with a leading bit is accepted: @f$\mathbf{0}@f$ is no decision,
/// @f$\mathbf{1}@f$ would conflict at once, and both are resampled. The
/// @f$\mathbf{1}@f$ branch is unreachable from the intended caller — it means
/// @f$\mathbf{1} \in \mathrm{span}(U) + \langle g_i \rangle@f$, exactly what the
/// satisfaction precondition rules out; that precondition is also what makes the
/// loop terminate, and debug builds check it.
SampledDecision sample_equation_from_clause(const Clause& clause, const RowSpace& trail_span,
                                            Rng& rng);

} // namespace xorcle
