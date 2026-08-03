#pragma once

/// @file leading_term_watchlist.h
/// @brief The clause selector that indexes clauses by the leading terms of
/// their watched rows.

#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "core/watch.h"
#include "heuristics/selection/clause_selector.h"

namespace xorcle {

/// @brief Queues only the clauses a newly learned equation could have affected.
///
/// Each non-unit clause is registered in the buckets of the leading columns of
/// its two watched rows, so learning an equation with leading column @f$c@f$
/// needs to wake only the clauses in the buckets that reduction can move: the
/// bucket for @f$c@f$ itself, and the bucket for every *pivot* column below
/// @f$c@f$, since a row leading on such a column can have its leading term
/// pushed up to @f$c@f$ by reduction.
///
/// @note The work list is a deque and is popped from the **back**. The bucket
/// for the new equation's own leading column is pushed to the back, so those
/// clauses are processed first; the buckets for lower pivot columns are pushed
/// to the front, so they are processed last. Everything else — new clauses,
/// asserting clauses, requeueing after a compaction — pushes to the back.
/// The 0.1 tree's committed version was a plain stack with a single push; the
/// deque split is the version that was ported, and the difference is observable
/// in the search order.
///
/// This is kept less for its speed than because a selector interface with only
/// `Cyclic` behind it would be a ten-hook abstraction serving one
/// implementation.
class LeadingTermWatchlist final : public ClauseSelector {
public:
    /// @brief Construct a watchlist over a given variable space.
    /// @param num_vars Number of active variable columns. There is one bucket
    /// per column including the constant column, so `num_vars + 1` in total.
    explicit LeadingTermWatchlist(std::size_t num_vars);

    /// @brief Pop the next active queued clause, or `std::nullopt` when the queue drains.
    std::optional<ClauseId> next() override;

    /// @brief Add an asserting clause and requeue it above every unit clause.
    void add_asserting_clause(Clause clause, const Trail& trail,
                              std::size_t removed_levels) override;

    /// @brief Queue the clauses a newly learned equation could have affected.
    void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) override;

    /// @brief Move a clause between leading-term buckets when its watch moves.
    void update_with_new_watch(ClauseId id, EquationId old_watch, WatchType type,
                               const Trail& trail) override;

    /// @brief Rebuild every bucket from the compacted clauses.
    void update_with_compaction(const CompactionTransform& transform, const Trail& trail) override;

protected:
    /// @brief Store a clause and register it in its leading-term buckets.
    /// @pre A non-unit clause has in-range, distinct watched-row indices.
    void add_clause(Clause clause) override;

private:
    /// @brief Queue a clause at the pop end, if it is not queued already.
    void push_top(ClauseId id);

    /// @brief Queue a clause at the far end, if it is not queued already.
    void push_bottom(ClauseId id);

    /// @brief `push_top()` for each id, in order.
    void append_top(const std::vector<ClauseId>& ids);

    /// @brief `push_bottom()` for each id, in order.
    void append_bottom(const std::vector<ClauseId>& ids);

    /// @brief Pop one clause from the pop end.
    /// @pre The queue is not empty.
    ClauseId pop();

    /// @brief The bucket a watched row belongs in, i.e. its leading column.
    /// @param clause The clause the row belongs to.
    /// @param row_id The watched row.
    std::size_t bucket_of(const Clause& clause, EquationId row_id) const;

    /// @brief Register a stored clause in the buckets of both its watched rows,
    /// or record it as a unit clause.
    /// @param id The clause id.
    void index_clause(ClauseId id);

    /// @brief Clause ids watching a row that leads on each column.
    std::vector<std::vector<ClauseId>> by_leading_term_;

    /// @brief The work list; popped from the back.
    std::deque<ClauseId> queue_;

    /// @brief Membership flags for `queue_`, parallel to `clauses_`.
    std::vector<bool> in_queue_;

    /// @brief Ids of the unit clauses, which have no watches to index.
    std::vector<ClauseId> unit_clauses_;
};

} // namespace xorcle
