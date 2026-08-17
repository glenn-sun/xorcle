#pragma once

/// @file leading_term_watchlist.h
/// @brief The selector that indexes clauses by their watched rows' leading terms.

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
/// Each non-unit clause is bucketed by the leading columns of its two watched
/// rows, so learning an equation with leading column @f$c@f$ wakes only the
/// buckets reduction can move: @f$c@f$ itself, and every *pivot* column below
/// @f$c@f$, since a row leading there can be pushed up to @f$c@f$.
///
/// @note The work list is a deque with **distinct top and bottom insertion**,
/// and the ordering is load-bearing. It pops from the back, where
/// `push_top()`/`append_top()` insert; `push_bottom()`/`append_bottom()` insert
/// at the other end. The bucket for the new equation's own leading column goes
/// to the top and is processed first; buckets for lower pivot columns go to the
/// bottom and are processed last. Everything else — new clauses, asserting
/// clauses, requeueing after compaction — goes to the top. Which end anything
/// lands on determines the search order.
class LeadingTermWatchlist final : public ClauseSelector {
public:
    /// @param num_vars One bucket per column, plus one for the constant column.
    explicit LeadingTermWatchlist(std::size_t num_vars);

    /// @brief Pop the next active queued clause, or `std::nullopt` when the queue drains.
    std::optional<ClauseId> next() override;

    /// @brief Add an asserting clause and requeue it above every unit clause.
    void add_asserting_clause(Clause clause, const Trail& trail,
                              std::size_t removed_levels) override;

    void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) override;

    /// @brief Move a clause between buckets when its watch moves.
    void update_with_new_watch(ClauseId id, EquationId old_watch, WatchType type,
                               const Trail& trail) override;

    void update_with_compaction(const CompactionTransform& transform, const Trail& trail) override;

protected:
    /// @brief Store a clause and register it in its leading-term buckets.
    /// @pre A non-unit clause has in-range, distinct watched rows.
    void add_clause(Clause clause) override;

private:
    void push_top(ClauseId id); ///< Queue at the pop end, unless already queued.
    void push_bottom(ClauseId id); ///< Queue at the far end, unless already queued.

    void append_top(const std::vector<ClauseId>& ids);
    void append_bottom(const std::vector<ClauseId>& ids);

    /// @pre The queue is not empty.
    ClauseId pop();

    /// @brief The bucket a watched row belongs in, i.e. its leading column.
    std::size_t bucket_of(const Clause& clause, EquationId row_id) const;

    /// @brief Bucket a stored clause by both watched rows, or record it as a unit.
    void index_clause(ClauseId id);

    std::vector<std::vector<ClauseId>> by_leading_term_; ///< Per column, the clauses watching it.

    std::deque<ClauseId> queue_; ///< The work list; popped from the back.
    std::vector<bool> in_queue_; ///< Membership flags for `queue_`, parallel to `clauses_`.
    std::vector<ClauseId> unit_clauses_; ///< Unit clauses, which have no watches to index.
};

} // namespace xorcle
