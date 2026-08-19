#include "heuristics/selection/leading_term_watchlist.h"

#include <algorithm>
#include <cassert>
#include <utility>

#include "algebra/reduce_result.h"
#include "util/column_set.h"

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

LeadingTermWatchlist::LeadingTermWatchlist(std::size_t num_vars)
    : by_leading_term_(num_vars + 1) {}

std::size_t LeadingTermWatchlist::bucket_of(const Clause& clause, ClauseRowIndex row_id) const {
    assert(row_id < clause.rows() && "watched row index out of range");
    const ReduceResult leading = clause.row(row_id).classify();
    // A stored clause row is never the true equation, and the constant column is
    // a legal bucket, so both surviving cases name a real column. The bucket
    // array is sized for the *initial* variable numbering and never shrunk.
    assert(leading.kind != ReduceResult::Kind::eq_true && "clause row is the true equation");
    const std::size_t bucket = leading.is_leading() ? leading.bit : clause.cols() - 1;
    assert(bucket < by_leading_term_.size() && "leading column out of bucket range");
    return bucket;
}

void LeadingTermWatchlist::push_top(ClauseId id) {
    assert(id < in_queue_.size());
    if (!in_queue_[id]) {
        queue_.push_back(id);
        in_queue_[id] = true;
    }
}

void LeadingTermWatchlist::push_bottom(ClauseId id) {
    assert(id < in_queue_.size());
    if (!in_queue_[id]) {
        queue_.push_front(id);
        in_queue_[id] = true;
    }
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

ClauseId LeadingTermWatchlist::pop() {
    assert(!queue_.empty());
    const ClauseId id = queue_.back();
    queue_.pop_back();
    in_queue_[id] = false;
    return id;
}

std::optional<ClauseId> LeadingTermWatchlist::next() {
    while (!queue_.empty()) {
        const ClauseId id = pop();
        if (is_clause_active(id)) {
            return id;
        }
    }
    return std::nullopt;
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
           "queue membership flags are desynchronized from clause storage");

    const ClauseId new_id = clauses_.size();
    debug_assert_non_unit_watches_valid(clause);
    ClauseSelector::add_clause(std::move(clause));
    in_queue_.push_back(false);

    index_clause(new_id);
    if (clauses_[new_id].rows() <= 1) {
        push_top(new_id);
    }

#ifndef NDEBUG
    debug_assert_storage_synchronized();
#endif
}

void LeadingTermWatchlist::add_asserting_clause(Clause clause, const Trail&, std::size_t) {
    add_clause(std::move(clause));

    // Everything queued before the conflict was queued against a trail that no
    // longer exists, so rebuild from the unit clauses with the new clause on top.
    queue_.clear();
    std::fill(in_queue_.begin(), in_queue_.end(), false);
    append_top(unit_clauses_);
    push_top(clauses_.size() - 1);
}

void LeadingTermWatchlist::update_with_new_equation(const Equation& reduced_eqn,
                                                    const Trail& trail) {
    const ReduceResult leading = reduced_eqn.classify();
    // The trail never learns the true equation, and the false equation is the
    // conflict the solver detects separately, so there is always a real column.
    assert(leading.is_leading() && "learned equation has no leading bit");
    assert(leading.bit < by_leading_term_.size() && "leading column out of bucket range");

    // Reduction can only lift a row's leading term up to `leading.bit`, and
    // only from a column that is itself a pivot; only those buckets need waking.
    std::size_t pivots_below = trail.span().pivot_columns().count_below(leading.bit);
    if (pivots_below != 0) {
        trail.span().pivot_columns().for_each_set([&](std::size_t column) {
            if (pivots_below == 0) {
                return; // ascending order, so every pivot below is already queued
            }
            --pivots_below;
            append_bottom(by_leading_term_[column]);
        });
    }
    append_top(by_leading_term_[leading.bit]);
}

void LeadingTermWatchlist::update_with_new_watch(ClauseId id, ClauseRowIndex old_watch,
                                                WatchType type, const Trail&) {
    assert(id < clauses_.size() && id < in_queue_.size());
    assert(is_clause_active(id));

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
           "queue membership flags are desynchronized from clause storage");

    for (std::vector<ClauseId>& bucket : by_leading_term_) {
        bucket.clear();
    }
    unit_clauses_.clear();

    const std::deque<ClauseId> old_queue = queue_;
    queue_.clear();
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
    debug_assert_storage_synchronized();
#endif
}

} // namespace xorcle
