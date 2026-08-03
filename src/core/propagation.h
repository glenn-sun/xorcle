#pragma once

#include <cstddef>
#include <optional>

#include "algebra/algebra.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "core/watch.h"

namespace xorcle {

/// @brief Unit propagation on one clause: the paper's Algorithm 3.
///
/// Given a clause and the current trail, this either moves a watch, finds that
/// the clause is already satisfied, or returns the equation the clause implies.
/// A conflict is not a separate outcome: an implied equation that happens to be
/// @f$\mathbf{1}@f$ *is* the conflict, and the caller discovers this by learning
/// it and then reducing @f$\mathbf{1}@f$ against the span.
///
/// Choosing *which* clause to propagate is a free choice the algorithm leaves
/// open, so it lives in `heuristics/selection/` rather than here.
/// The loop that asks a selector for clauses
/// until one propagates belongs with the selector and the solver.
///
/// @note The two scratch equations are long-lived members refilled with
/// `Equation::assign_from()`, never freshly constructed. This preallocated-
/// scratch discipline, not the copy itself, is where the old tree's
/// `unsafe_copy` win came from, so
/// `unit_propagate_one_clause()` returns a pointer into that scratch rather than
/// an `Equation` by value.
class PropagationEngine {
public:
    /// @brief Construct an engine for a given variable count.
    /// @param backend Storage representation to use for the scratch equations.
    /// @param num_vars Number of variable columns. The constant column is added internally.
    PropagationEngine(Algebra backend, std::size_t num_vars);

    /// @brief Resize the internal scratch for a new active variable count.
    ///
    /// Called after a compaction rewrites the problem into fewer variables.
    /// @param num_vars The new number of variable columns.
    void reconfigure(std::size_t num_vars);

    /// @brief Number of variable columns this engine is configured for.
    std::size_t num_vars() const noexcept { return num_vars_; }

    /// @brief Attempt to propagate from a single clause.
    /// @param id Identifier of `clause`, reported to `observer`.
    /// @param clause The clause to propagate from, in its linear negation. Its
    /// watches may be updated.
    /// @param trail The current trail.
    /// @param zero_watch_hint A trigger the selector already computed, if it
    /// tracks reductions itself; otherwise empty, and the engine recomputes.
    /// @param num_watch_updates Incremented once per watch moved.
    /// @param observer Notified once per watch moved.
    /// @return A pointer to the implied equation, or `nullptr` if the clause
    /// implies nothing right now. The equation is engine-owned scratch, valid
    /// until the next call on this engine, and may be modified by the caller.
    /// @pre `clause.cols() == num_vars() + 1` and `clause.rows() >= 1`.
    /// @pre `zero_watch_hint` is empty when the clause is unit.
    /// @pre A non-empty `zero_watch_hint` names a trigger that currently holds.
    /// @note `WatchType::sum` hints are normalized to updating `watched1`.
    Equation* unit_propagate_one_clause(ClauseId id, Clause& clause, const Trail& trail,
                                        std::optional<WatchType> zero_watch_hint,
                                        std::size_t& num_watch_updates, WatchObserver& observer);

private:
#ifndef NDEBUG
    /// @brief Assert that a hinted trigger really holds for `clause`.
    ///
    /// Overwrites both scratch equations, which is safe because the hint path
    /// derives its task from the hint alone and refills them afterwards.
    void debug_assert_watch_hint_holds(const Clause& clause, const Trail& trail, WatchType hint);
#endif

    std::size_t num_vars_;

    /// @brief Scratch for the first watch's reduction, and for candidate
    /// replacement rows.
    Equation working_eqn_;

    /// @brief Scratch for the second watch's reduction, and then for the
    /// complement watch's reduction, which overwrites it.
    Equation working_eqn2_;

    /// @brief The false equation @f$\mathbf{1}@f$, preallocated.
    ///
    /// Adding it to a row is the linear negation @f$g \mapsto g + \mathbf{1}@f$
    /// that turns a falsified clause row back into the equation the clause
    /// implies. It is a whole-row operation, so it stays inside the coarse
    /// algebra interface.
    Equation false_equation_;
};

} // namespace xorcle
