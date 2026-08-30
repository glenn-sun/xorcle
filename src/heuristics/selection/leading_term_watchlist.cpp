#include "heuristics/selection/leading_term_watchlist.h"

#include <algorithm>
#include <cassert>
#include <utility>

#include "algebra/reduce_result.h"
#include "util/index_set.h"

namespace xorcle {

namespace {

void debug_assert_non_unit_watches_valid(const Clause& clause) {
    if (clause.rows() <= 1) {
        return;
    }
    assert(clause.watched0 < clause.rows() && clause.watched1 < clause.rows() &&
           "watched row index out of range");
    assert(clause.watched0 != clause.watched1 && "non-unit clause reuses a watched row");
}

} // namespace

LeadingTermWatchlist::LeadingTermWatchlist(std::size_t num_vars, std::size_t defer_pushes)
    : by_leading_term_(num_vars + 1), defer_pushes_(defer_pushes) {
    assert(defer_pushes >= 1 && "a clause is queued on at least its first bottom push");
}

std::size_t LeadingTermWatchlist::bucket_of(const Clause& clause, ClauseRowIndex row_idx) const {
    // Stronger than `RowEchelonMatrix::row()`'s own bound check below: this says
    // the clause's *stored watch* is in range.
    assert(row_idx < clause.rows() && "watched row index out of range");
    const ReduceResult leading = clause.row(row_idx).classify();
    // A stored clause row is never the true equation, and the constant column is
    // a legal bucket, so both surviving cases name a real column. The bucket
    // array is sized for the *initial* variable numbering and never shrunk.
    assert(leading.kind != ReduceResult::Kind::eq_true && "clause row is the true equation");
    const std::size_t bucket = leading.is_leading() ? leading.bit : clause.cols() - 1;
    assert(bucket < by_leading_term_.size() && "leading column out of bucket range");
    return bucket;
}

void LeadingTermWatchlist::push_top(ClauseId id) {
    assert(id < in_queue_.size() && "clause id is out of range");
    if (!in_queue_[id]) {
        queue_.push_back(id);
        in_queue_[id] = true;
    }
}

void LeadingTermWatchlist::push_bottom(ClauseId id) {
    assert(id < in_queue_.size() && "clause id is out of range");
    if (in_queue_[id]) {
        return;
    }
    if (defer_pushes_ > 1) {
        const std::uint32_t pushes = ++bottom_pushes_since_pop_[id];
        if (pushes == 1) {
            deferred_.push_back(id);
        }
        if (pushes < defer_pushes_) {
            return;
        }
    }
    queue_.push_front(id);
    in_queue_[id] = true;
}

std::optional<ClauseId> LeadingTermWatchlist::pop_deferred() {
    while (deferred_pos_ < deferred_.size()) {
        const ClauseId id = deferred_[deferred_pos_++];
        if (bottom_pushes_since_pop_[id] == 0) {
            continue;
        }
        assert(!in_queue_[id] && "a deferred clause is also in the queue");
        return id;
    }
    assert(std::all_of(bottom_pushes_since_pop_.begin(), bottom_pushes_since_pop_.end(),
                       [](std::uint32_t pushes) { return pushes == 0; }) &&
           "a clause kept a push count no deferred entry names");
    deferred_.clear();
    deferred_pos_ = 0;
    return std::nullopt;
}

void LeadingTermWatchlist::clear_deferred() {
    for (const ClauseId id : deferred_) {
        bottom_pushes_since_pop_[id] = 0;
    }
    deferred_.clear();
    deferred_pos_ = 0;
}

void LeadingTermWatchlist::append_top(const std::vector<ClauseId>& ids) {
    for (const ClauseId id : ids) {
        push_top(id);
    }
}

void LeadingTermWatchlist::append_bottom(const std::vector<ClauseId>& ids) {
    for (const ClauseId id : ids) {
        push_bottom(id);
    }
}

std::optional<ClauseId> LeadingTermWatchlist::pop() {
    if (queue_.empty()) {
        return pop_deferred();
    }
    const ClauseId id = queue_.back();
    queue_.pop_back();
    in_queue_[id] = false;
    return id;
}

std::optional<ClauseId> LeadingTermWatchlist::next() {
    while (true) {
        const std::optional<ClauseId> id = pop();
        if (!id.has_value()) {
            return std::nullopt;
        }
        if (defer_pushes_ > 1) {
            bottom_pushes_since_pop_[*id] = 0;
        }
        if (is_clause_active(*id)) {
            return id;
        }
    }
}

void LeadingTermWatchlist::index_clause(ClauseId id) {
    const Clause& stored = clauses_[id];
    if (stored.rows() > 1) {
        debug_assert_non_unit_watches_valid(stored);
        by_leading_term_[bucket_of(stored, stored.watched0)].push_back(id);
        by_leading_term_[bucket_of(stored, stored.watched1)].push_back(id);
    } else {
        unit_clauses_.push_back(id);
    }
}

void LeadingTermWatchlist::add_clause(Clause clause) {
    assert(in_queue_.size() == clauses_.size() &&
           "queue membership flags are desynchronized from the clause database");

    const ClauseId new_id = clauses_.size();
    // The watch check runs in `index_clause()` below, on the stored clause.
    ClauseSelector::add_clause(std::move(clause));
    in_queue_.push_back(false);
    bottom_pushes_since_pop_.push_back(0);

    index_clause(new_id);
    if (clauses_[new_id].rows() <= 1) {
        push_top(new_id);
    }

#ifndef NDEBUG
    debug_assert_database_synchronized();
#endif
}

void LeadingTermWatchlist::add_asserting_clause(Clause clause, const Trail& trail,
                                                const AssertingClauseInfo& info) {
    ClauseSelector::add_asserting_clause(std::move(clause), trail, info);

    // Everything queued before the conflict was queued against a trail that no
    // longer exists, so rebuild from the unit clauses with the new clause on top.
    queue_.clear();
    clear_deferred();
    std::fill(in_queue_.begin(), in_queue_.end(), false);
    append_top(unit_clauses_);
    push_top(clauses_.size() - 1);
}

void LeadingTermWatchlist::update_with_new_equation(const Equation& reduced_eqn,
                                                    const Trail& trail) {
    const ReduceResult leading = reduced_eqn.classify();
    // The trail never learns the true equation; the false equation is the
    // conflicting propagation, and buckets to the constant column as a row does.
    assert(leading.kind != ReduceResult::Kind::eq_true &&
           "learned equation is the true equation");
    const std::size_t bucket = leading.is_leading() ? leading.bit : reduced_eqn.num_bits() - 1;
    assert(bucket < by_leading_term_.size() && "leading column out of bucket range");

    // Reduction can only lift a row's leading term up to `bucket`, and only
    // from a column that is itself a pivot; only those buckets need waking.
    std::size_t pivots_below = trail.span().pivot_col_idxs().count_below(bucket);
    if (pivots_below != 0) {
        trail.span().pivot_col_idxs().for_each_set([&](std::size_t col_idx) {
            if (pivots_below == 0) {
                return; // ascending order, so every pivot below is already queued
            }
            --pivots_below;
            assert(col_idx < by_leading_term_.size() &&
                   "the trail carries more columns than the watchlist was built for");
            append_bottom(by_leading_term_[col_idx]);
        });
    }
    append_top(by_leading_term_[bucket]);
}

void LeadingTermWatchlist::update_with_new_watch(ClauseId id, ClauseRowIndex old_watch,
                                                WatchType type, const Trail&) {
    assert(id < in_queue_.size() && "clause id is out of range");
    // The clause-database bound is checked by `is_clause_active()`.
    assert(is_clause_active(id) && "a watch moved on an inactive clause");

    const Clause& moved = clauses_[id];
    debug_assert_non_unit_watches_valid(moved);

    std::vector<ClauseId>& old_bucket = by_leading_term_[bucket_of(moved, old_watch)];
    std::erase(old_bucket, id);

    // The clause's watches were already updated by the propagator, so reading
    // them here gives the row the watch moved *to*.
    ClauseRowIndex new_watch = 0;
    switch (type) {
        case WatchType::zero:
            new_watch = moved.watched0;
            break;
        case WatchType::one:
            new_watch = moved.watched1;
            break;
        case WatchType::sum:
            assert(false && "sum triggers are normalized to one before reaching a selector");
            return;
    }
    by_leading_term_[bucket_of(moved, new_watch)].push_back(id);
}

void LeadingTermWatchlist::update_with_compaction(const CompactionTransform&, const Trail&) {
    assert(in_queue_.size() == clauses_.size() &&
           "queue membership flags are desynchronized from the clause database");

    for (std::vector<ClauseId>& bucket : by_leading_term_) {
        bucket.clear();
    }
    unit_clauses_.clear();

    const std::deque<ClauseId> old_queue = queue_;
    queue_.clear();
    clear_deferred();
    std::fill(in_queue_.begin(), in_queue_.end(), false);

    for (ClauseId id = 0; id < clauses_.size(); ++id) {
        if (is_clause_active(id)) {
            index_clause(id);
        }
    }

    for (const ClauseId id : old_queue) {
        if (id < clauses_.size() && is_clause_active(id)) {
            push_top(id);
        }
    }
    append_top(unit_clauses_);

#ifndef NDEBUG
    debug_assert_database_synchronized();
#endif
}

} // namespace xorcle
