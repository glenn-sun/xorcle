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
/// A conflict is not a separate outcome: an implied equation that happens to be
/// @f$\mathbf{1}@f$ *is* the conflict, and the caller discovers this by learning
/// it and then reducing @f$\mathbf{1}@f$ against the span. Choosing *which*
/// clause to propagate is a free choice and lives in `heuristics/selection/`.
///
/// @note The scratch equations are long-lived members refilled with
/// `assign_from()`, never freshly constructed, which is why
/// `unit_propagate_one_clause()` returns a pointer into that scratch.
class Propagator {
public:
    Propagator(EquationRepr repr, std::size_t num_vars);

    /// @brief Resize the internal scratch, after a compaction rewrites the
    /// problem into fewer variables.
    void reconfigure(std::size_t num_vars);

    /// @brief Attempt to propagate from a single clause, in its linear negation
    /// and with its watches possibly updated. `zero_watch_hint` is a trigger the
    /// selector already computed, if it tracks reductions itself; otherwise it is
    /// empty and this recomputes it. `num_watch_updates` is incremented and
    /// `handler` notified once per watch moved.
    ///
    /// @return A pointer to the implied equation, or `nullptr` if the clause
    /// implies nothing right now. The equation is this propagator's own scratch,
    /// valid until its next call, and may be modified by the caller.
    /// @pre `clause.cols() == trail.span().cols()`, matching this propagator's
    /// own variable count, and `clause.repr() == trail.repr()`.
    /// @pre `clause.rows() >= 1`; a non-unit clause has two distinct watches in
    /// range and `next_watch_search_start < clause.rows()`.
    /// @pre A non-empty `zero_watch_hint` names a trigger that currently holds,
    /// and it is empty when the clause is unit. `WatchType::sum` hints are
    /// normalized to updating `watched1`.
    ///
    /// @note `num_watch_updates` is an accumulator the solver owns rather than a
    /// returned count, and is the one out-parameter here that is genuinely on the
    /// propagation path: on `random_kxnf_n_n18_k4_s5` this function runs
    /// 1,595,421 times against 1,427 conflicts and 1,436 decisions, so it is
    /// called roughly 1,100x more often than any conflict- or decision-scale work.
    Equation* unit_propagate_one_clause(ClauseId id, Clause& clause, const Trail& trail,
                                        std::optional<WatchType> zero_watch_hint,
                                        std::size_t& num_watch_updates,
                                        WatchUpdateHandler& handler);

private:
#ifndef NDEBUG
    /// @brief Assert that a hinted trigger really holds. Overwrites both scratch
    /// equations, which is safe because the hint path derives its task from the
    /// hint alone and refills them afterwards.
    void debug_assert_watch_hint_holds(const Clause& clause, const Trail& trail, WatchType hint);
#endif

    std::size_t num_vars_;

    Equation working_eqn_;  ///< First watch's reduction, then candidate rows.
    Equation working_eqn2_; ///< Second watch's reduction, then the complement's.

    /// @brief A preallocated @f$\mathbf{1}@f$. Adding it to a row is the linear
    /// negation @f$g \mapsto g + \mathbf{1}@f$ that turns a falsified clause row
    /// back into the equation the clause implies.
    Equation false_equation_;
};

} // namespace xorcle
