#pragma once

#include <cstddef>
#include <optional>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "core/watch.h"

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
    /// @param num_watch_updates Incremented once per watch moved. An accumulator
    /// rather than a returned count: this runs on the propagation path, roughly
    /// 1,000x more often than conflict- or decision-scale work.
    /// @param handler Notified once per watch moved.
    ///
    /// @return The implied equation, or `nullptr` if the clause implies nothing.
    /// It is this propagator's own scratch, valid until its next call and free
    /// for the caller to modify.
    /// @pre `clause.cols() == trail.span().cols()`, matching this propagator's
    /// own variable count, and `clause.repr() == trail.repr()`.
    /// @pre `clause.rows() >= 1`; a non-unit clause has two distinct watches in
    /// range and `next_watch_search_start < clause.rows()`.
    /// @pre A non-empty `trigger_hint` names a trigger that holds, and it is
    /// empty when the clause is unit. `WatchType::sum` normalizes to `watched1`.
    Equation* unit_propagate_one_clause(ClauseId id, Clause& clause, const Trail& trail,
                                        std::optional<WatchType> trigger_hint,
                                        std::size_t& num_watch_updates,
                                        WatchUpdateHandler& handler);

private:
#ifndef NDEBUG
    /// @brief Overwrites both scratch equations.
    void debug_assert_watch_hint_holds(const Clause& clause, const Trail& trail, WatchType hint);
#endif

    std::size_t num_vars_;

    Equation working_eqn_;  ///< First watch's reduction, then candidate rows.
    Equation working_eqn2_; ///< Second watch's reduction, then the complement's.

    /// @brief A preallocated `0 = 1`; adding it to a falsified clause row is the
    /// linear negation that yields the equation the clause implies.
    Equation false_equation_;
};

} // namespace xorcle
