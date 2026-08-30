#pragma once

/// @file leading_term_watchlist.h
/// @brief The selector that indexes clauses by their watched rows' leading terms.

#include <cstddef>
#include <cstdint>
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
/// Each non-unit clause is bucketed by the leading columns of its two watched rows; see
/// arXiv:2605.15002.
///
/// @note The bucket for a new equation's own leading column is queued at the top; buckets
/// for lower pivot columns at the bottom, everything else at the top. Which end anything
/// lands on determines the search order.
class LeadingTermWatchlist final : public ClauseSelector {
public:
    /// @param num_vars One bucket per column, plus one for the constant column.
    /// @param defer_pushes Bottom pushes a clause takes before it joins the queue; a clause
    /// still short of that many is served once the queue is empty, in the order it was
    /// first pushed.
    /// @pre `defer_pushes` is at least 1.
    explicit LeadingTermWatchlist(std::size_t num_vars, std::size_t defer_pushes = 1);

    std::optional<ClauseId> next() override;

    /// @brief Add an asserting clause and requeue it above every unit clause.
    void add_asserting_clause(Clause clause, const Trail& trail,
                              const AssertingClauseInfo& info) override;

    /// @pre `reduced_eqn` is not the true equation, and `trail` is over the numbering it
    /// was constructed with.
    void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) override;

    /// @pre `id` names an active clause and `old_watch < clause(id).rows()`.
    void update_with_new_watch(ClauseId id, ClauseRowIndex old_watch, WatchType type,
                               const Trail& trail) override;

    void update_with_compaction(const CompactionTransform& transform, const Trail& trail) override;

protected:
    /// @pre A non-unit clause has in-range, distinct watched rows.
    void add_clause(Clause clause) override;

private:
    void push_top(ClauseId id); ///< Queue at the pop end, unless already queued.
    void push_bottom(ClauseId id); ///< Queue at the far end, on the `defer_pushes_`-th push.

    /// @brief The next clause held short of `defer_pushes_`, skipping the entries whose
    /// clause has since been popped.
    std::optional<ClauseId> pop_deferred();

    void clear_deferred();

    void append_top(const std::vector<ClauseId>& ids);
    void append_bottom(const std::vector<ClauseId>& ids);

    /// @brief The next clause to examine, or `std::nullopt` once nothing is left.
    std::optional<ClauseId> pop();

    /// @pre `row_idx < clause.rows()`, and that row is not the true equation.
    std::size_t bucket_of(const Clause& clause, ClauseRowIndex row_idx) const;

    /// @brief Bucket a clause by both watched rows, or record it as a unit.
    void index_clause(ClauseId id);

    std::vector<std::vector<ClauseId>> by_leading_term_; ///< Per column, the clauses watching it.

    std::deque<ClauseId> queue_; ///< Popped from the back.
    std::vector<bool> in_queue_;
    std::vector<ClauseId> unit_clauses_;

    std::size_t defer_pushes_;
    std::vector<std::uint32_t> bottom_pushes_since_pop_;
    std::vector<ClauseId> deferred_; ///< In first-push order; entries go stale rather than move.
    std::size_t deferred_pos_ = 0;
};

} // namespace xorcle
