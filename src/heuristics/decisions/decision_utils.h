#pragma once

/// @file decision_utils.h
/// @brief Shared machinery the decision procedures build on: the reusable
/// clause-satisfaction test, basis scanning, basis projection, and sampling an
/// equation out of a clause.
///
/// This is heuristic support rather than a general utility, so it lives beside
/// the procedures that use it rather than in `util/` where the 0.1 tree kept it.

#include <cstddef>
#include <vector>

#include "algebra/algebra.h"
#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "util/random.h"

namespace xorcle {

/// @brief Reusable scratch for `clause_satisfied_by_trail()`.
///
/// The satisfaction test builds a small echelon basis out of the clause's
/// reduced rows, once per call. That basis, and the two equations fed through
/// it, are the whole cost of the test, so they are held here and refilled with
/// `Equation::assign_from()` rather than reallocated per call.
///
/// @note The 0.1 tree hand-rolled the local basis as three parallel flat arrays
/// plus a private `partial_reduce_by_local_basis`. It is a `RowSpace` here: the
/// row-echelon algorithms already exist, are tested against both backends, and
/// behave identically, because both store each new row partially reduced by the
/// rows already present and both reduce only while the leading column is a
/// pivot.
class ClauseSatisfactionWorkspace {
public:
    /// @brief Construct a workspace for a given active variable count.
    /// @param backend Storage representation to use.
    /// @param num_vars Number of active variable columns.
    ClauseSatisfactionWorkspace(Algebra backend, std::size_t num_vars);

    /// @brief Resize every buffer for a new active variable count.
    /// @param num_vars The new number of active variable columns.
    void reconfigure(std::size_t num_vars);

    /// @brief Number of active variable columns this workspace is sized for.
    std::size_t num_vars() const noexcept { return num_vars_; }

private:
    friend bool clause_satisfied_by_trail(const RowSpace& trail_span, const Clause& clause,
                                          ClauseSatisfactionWorkspace& workspace);

    /// @brief Empty the local basis, keeping the storage it has already grown into.
    void clear_local_basis();

    Algebra backend_;
    std::size_t num_vars_;

    /// @brief A preallocated @f$\mathbf{1}@f$; a constant, not scratch.
    Equation false_equation_;

    /// @brief @f$\mathbf{1}@f$ reduced by the trail span and then, incrementally,
    /// by each clause row added to the local basis.
    Equation scratch_false_;

    /// @brief One clause row, reduced by the trail span.
    Equation scratch_row_;

    /// @brief The clause's reduced rows, echelonized.
    RowSpace local_basis_;
};

/// @brief Choose the first basis equation the trail has not already decided.
/// @param basis Candidate rows, scanned in order.
/// @param trail_span The current @f$\mathrm{span}(U)@f$.
/// @param reduced_scratch Preallocated scratch of width `trail_span.cols()`,
/// overwritten with the reduction of each candidate in turn.
/// @return The first candidate whose reduction has a leading column, i.e. the
/// first one that is a legal decision. The returned reference points into
/// `basis` and is *not* reduced.
/// @pre Every equation in `basis` has width `trail_span.cols()`, and
/// `reduced_scratch` does too.
/// @pre The caller has established that some legal decision remains.
/// @note The 0.1 tree had two byte-identical copies of this, `decide_from_basis`
/// and `decide_from_units`, differing only in a parameter name.
const Equation& decide_from_basis(const std::vector<Equation>& basis, const RowSpace& trail_span,
                                  Equation& reduced_scratch);

/// @brief Whether the trail already satisfies a clause.
/// @param trail_span The current @f$\mathrm{span}(U)@f$.
/// @param clause The clause to test, in its linear negation.
/// @param workspace Reusable scratch, sized for the active variable count.
/// @return True if the clause is satisfied under every extension of the trail.
/// @pre `workspace.num_vars() + 1 == trail_span.cols()`.
///
/// A clause @f$\bigwedge_i g_i@f$ is *falsified* exactly when every @f$g_i@f$
/// lies in @f$\mathrm{span}(U)@f$, and *satisfied* exactly when @f$\mathbf{1}
/// \in \mathrm{span}(U) + \langle g_1, \dots, g_k \rangle@f$. This computes the
/// second condition incrementally, stopping as soon as it holds.
bool clause_satisfied_by_trail(const RowSpace& trail_span, const Clause& clause,
                               ClauseSatisfactionWorkspace& workspace);

/// @brief Project a literal basis through a compaction, preserving basis order.
/// @param basis Candidate literal basis, in old active-variable order.
/// @param transform The compaction step to apply.
/// @param backend Storage representation for the unit equations it has to add.
/// @return A complete literal basis over the compacted variables: the first
/// projected equation for each new leading column, in old basis order, followed
/// by a unit equation for every new variable no projection covered.
/// @pre Every equation in `basis` has width `transform.old_num_vars() + 1`.
/// @post The result has exactly `transform.new_num_vars()` equations, one per
/// leading column.
std::vector<Equation> project_basis_keep_order(const std::vector<Equation>& basis,
                                               const CompactionTransform& transform,
                                               Algebra backend);

/// @brief Sample a random combination of a clause's rows that is a legal decision.
/// @param clause The clause to sample from, in its linear negation.
/// @param trail_span The current @f$\mathrm{span}(U)@f$.
/// @param rng RNG stream used to choose the combination of rows.
/// @param out_candidate Receives the sampled combination, unreduced.
/// @param out_reduced Receives its partial reduction by `trail_span`.
/// @return True once a legal decision was sampled.
/// @pre `clause.rows() > 0`, and `out_candidate` / `out_reduced` have width
/// `trail_span.cols()`.
/// @pre The clause is not satisfied by the trail, i.e.
/// `clause_satisfied_by_trail()` returned false for it.
///
/// This is rejection sampling, so the number of RNG draws is data-dependent;
/// `random_nonzero_coefficients()` documents the per-attempt draw pattern that
/// the differential harness depends on.
///
/// @note **Contract decision.** The 0.1 tree accepted any combination whose
/// reduction was not @f$\mathbf{0}@f$, which included @f$\mathbf{1}@f$ — a
/// decision that conflicts the instant it is made, and one that
/// `Solver::make_decision_step` then asserted against. The two sites disagreed.
/// This resolves it in favour of the assert: only a reduction with a leading
/// column is accepted. That is not a behaviour change, because a combination
/// reducing to @f$\mathbf{1}@f$ means @f$\mathbf{1} \in \mathrm{span}(U) +
/// \langle g_i \rangle@f$, which is exactly the condition the caller's
/// `clause_satisfied_by_trail()` guard has already ruled out. The precondition
/// above is what makes the rejection loop terminate, and a debug build checks
/// it directly.
bool sample_equation_from_clause(const Clause& clause, const RowSpace& trail_span, Rng& rng,
                                 Equation& out_candidate, Equation& out_reduced);

} // namespace xorcle
