#pragma once

#include <cstddef>
#include <optional>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/reduce_result.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "core/watch.h"
#include "util/index_set.h"

namespace xorcle {

/// @brief Unit propagation on one clause: either move a watch, find the clause
/// already satisfied, or return the equation the clause implies.
///
/// A conflict is not a separate outcome: an implied `0 = 1` *is* the conflict.
class Propagator {
public:
    Propagator(EquationRepr repr, std::size_t num_vars);

    /// @brief Resize the internal scratch after a compaction.
    void reconfigure(std::size_t num_vars);

    /// @brief Attempt to propagate from a single clause, in its linear negation
    /// and with its watches possibly updated.
    ///
    /// @param trigger_hint A trigger the selector already computed, or empty.
    /// @param num_watch_updates Incremented once per watch moved.
    /// @param handler Asked for the reduction a hinted trigger leaves standing, and
    /// notified once per watch moved.
    /// @param used_row_idxs When non-null and the clause propagates, receives the
    /// trail-span rows the propagated equation's derivation uses, indexed by span
    /// row, resized to the span's row count. Filled by a pass of its own, which
    /// fewer than 0.5% of attempts reach.
    ///
    /// @return The implied equation, or `nullptr` if the clause implies nothing.
    /// It is this propagator's own scratch, valid until its next call and free
    /// for the caller to modify.
    /// @pre `clause.cols() == trail.span().cols()`, matching this propagator's
    /// own variable count, and `clause.repr() == trail.repr()`.
    /// @pre `clause.rows() >= 1`; a non-unit clause has two distinct watches in
    /// range and `next_watch_search_start < clause.rows()`.
    /// @pre A non-empty `trigger_hint` names a trigger that holds, its companion
    /// watch is not `0 = 1` modulo the span, and it is empty when the clause is
    /// unit. A `WatchType::one` hint may name a second watch in the span plus the first.
    Equation* unit_propagate_one_clause(ClauseId id, Clause& clause, const Trail& trail,
                                        std::optional<WatchType> trigger_hint,
                                        std::size_t& num_watch_updates,
                                        WatchUpdateHandler& handler,
                                        IndexSet* used_row_idxs = nullptr);

    /// @brief Whether the last call found the clause already satisfied, which is
    /// the one `nullptr` the caller can remember: a satisfied clause stays
    /// satisfied until the trail backjumps below the level that satisfied it.
    bool last_attempt_found_satisfied() const noexcept { return found_satisfied_; }

    /// @brief The row that satisfied the clause, valid only where
    /// `last_attempt_found_satisfied()` holds.
    std::size_t last_satisfying_row_idx() const noexcept { return satisfying_row_idx_; }

    /// @brief The clause row the last call propagated from, valid only where it
    /// returned an equation.
    std::size_t last_propagating_row_idx() const noexcept { return propagating_row_idx_; }

private:
    /// @brief Fill `used_row_idxs` with the trail-span rows the propagation from
    /// `propagating_row_idx` rests on.
    ///
    /// Every other row is either falsified, and owes the trail its own reduction,
    /// or equal to the propagating row modulo the span, and owes it their sum.
    /// The propagating row owes nothing unless it is falsified too, which is the
    /// conflict.
    void collect_used_rows(const Clause& clause, const Trail& trail,
                           std::size_t propagating_row_idx, bool propagating_row_is_falsified,
                           IndexSet& used_row_idxs);

#ifndef NDEBUG
    /// @brief Overwrites both scratch equations.
    void debug_assert_watch_hint_holds(const Clause& clause, const Trail& trail, WatchType hint);

    /// @brief Check what a handler copied out against reducing `row` afresh.
    static void debug_assert_is_held_reduction(const Equation& row, const Trail& trail,
                                               const Equation& reduced, const ReduceResult& result,
                                               HeldReduction held);
#endif

    std::size_t num_vars_;

    Equation working_eqn_;  ///< First watch's reduction, then candidate rows.
    Equation working_eqn2_; ///< Second watch's reduction, then the complement's.
    Equation working_eqn3_; ///< A candidate row reduced on by the complement.
    Equation used_rows_eqn_;  ///< Scratch for `collect_used_rows()`.
    Equation used_rows_eqn2_; ///< The propagating row's residue, for the same.

    /// @brief One clause row's coefficients, before they join `used_row_idxs`.
    IndexSet row_coefficients_;

    /// @brief A preallocated `0 = 1`; adding it to a falsified clause row undoes
    /// the linear negation, recovering the equation the clause implies.
    Equation false_equation_;

    bool found_satisfied_ = false;
    std::size_t satisfying_row_idx_ = 0;
    std::size_t propagating_row_idx_ = 0;
};

} // namespace xorcle
