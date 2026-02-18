#include "heuristics/ExactWatchlist.h"

#include <algorithm>
#include <cassert>

#include "bit/vector.h"

namespace heuristics {

ExactWatchlistSnapshot::ExactWatchlistSnapshot(std::size_t num_vars)
    : bucket_head(num_vars + 1) {}

void ExactWatchlistSnapshot::add_to_bucket(WatchedId id, std::size_t bucket) {
    if (bucket_head[bucket].has_value()) {
        bucket_prev[bucket_head[bucket]] = id;
    }
    bucket_next[id] = bucket_head[bucket];
    bucket_prev[id] = WatchedId();
    which_bucket[id] = bucket;
    bucket_head[bucket] = id;
}

void ExactWatchlistSnapshot::move_to_bucket(WatchedId id, std::size_t new_bucket) {
    if (new_bucket == which_bucket[id]) {
        return;
    }

    // (A) -- id -- (B)
    if (bucket_prev[id].has_value()) {
        // A -> B
        bucket_next[bucket_prev[id]] = bucket_next[id];
    } else {
        // id was head
        // Now B is head
        bucket_head[which_bucket[id]] = bucket_next[id];
    }

    if (bucket_next[id].has_value()) {
        // B -> A
        bucket_prev[bucket_next[id]] = bucket_prev[id];
    }

    add_to_bucket(id, new_bucket);
}

void ExactWatchlistSnapshot::init_in_bucket(const Equation& eqn, std::size_t bucket) {
    equations.push_back(eqn);
    bucket_next.push_back(WatchedId());
    bucket_prev.push_back(WatchedId());
    which_bucket.push_back(0);
    add_to_bucket(equations.size() - 1, bucket);
}

void ExactWatchlistSnapshot::recompute_buckets(const std::vector<Clause>& clauses, const Trail& trail, std::size_t num_vars) {
    std::fill(equations.begin(), equations.end(), Equation());
    std::fill(bucket_next.begin(), bucket_next.end(), WatchedId());
    std::fill(bucket_prev.begin(), bucket_prev.end(), WatchedId());
    std::fill(bucket_head.begin(), bucket_head.end(), WatchedId());
    std::fill(which_bucket.begin(), which_bucket.end(), 0);

    for (ClauseId id = 0; id < clauses.size(); ++id) {
        const Clause& c = clauses[id];

        if (c.rows() > 1) {
            WatchedId wid_zero = WatchedId(id, WatchType::Zero);
            WatchedId wid_one = WatchedId(id, WatchType::One);
            WatchedId wid_sum = WatchedId(id, WatchType::Sum);

            equations[wid_zero] = c.row(c.watched0);
            trail.span().reduce(equations[wid_zero]);
            std::size_t zero_first_set = equations[wid_zero].first_set();
            if (zero_first_set == bit::vector<>::npos) {
                zero_first_set = num_vars;
            }
            add_to_bucket(wid_zero, zero_first_set);

            equations[wid_one] = c.row(c.watched1);
            trail.span().reduce(equations[wid_one]);
            std::size_t one_first_set = equations[wid_one].first_set();
            if (one_first_set == bit::vector<>::npos) {
                one_first_set = num_vars;
            }
            add_to_bucket(wid_one, one_first_set);

            equations[wid_sum] = c.row(c.watched0) + c.row(c.watched1);
            trail.span().reduce(equations[wid_sum]);
            std::size_t sum_first_set = equations[wid_sum].first_set();
            if (sum_first_set == bit::vector<>::npos) {
                sum_first_set = num_vars;
            }
            add_to_bucket(wid_sum, sum_first_set);
        }
    }
}

ExactWatchlist::ExactWatchlist(std::size_t num_vars)
    : ClauseSelector(),
      num_vars(num_vars),
      snapshots({ExactWatchlistSnapshot(num_vars)}),
      working_eqn(num_vars + 1) {}

ClauseId ExactWatchlist::get_pop() {
    ClauseId id = stack.back();
    stack.pop_back();
    return id;
}

std::optional<ClauseId> ExactWatchlist::next() {
    if (stack.empty()) {
        return std::nullopt;
    }
    return get_pop();
}

void ExactWatchlist::add_starting_clause(const Clause& clause) {
    if (clause.rows() > 1) {
        snapshots.back().init_in_bucket(clause.row(clause.watched0), clause.row(clause.watched0).first_set());
        snapshots.back().init_in_bucket(clause.row(clause.watched1), clause.row(clause.watched1).first_set());

        working_eqn.unsafe_copy(clause.row(clause.watched0));
        working_eqn += clause.row(clause.watched1);
        snapshots.back().init_in_bucket(working_eqn, working_eqn.first_set());
    } else {
        // Add placeholders
        snapshots.back().init_in_bucket(Equation(), num_vars);
        snapshots.back().init_in_bucket(Equation(), num_vars);
        snapshots.back().init_in_bucket(Equation(), num_vars);

        unit_clauses.push_back(clauses.size());
        stack.push_back(clauses.size());
    }
    clauses.push_back(clause);
}

void ExactWatchlist::add_asserting_clause(const Clause& clause, const Trail& trail, std::size_t /*jumpable_levels*/) {
    add_starting_clause(clause);

    // Reset queue and add unit clauses/new asserting clause
    stack.clear();
    for (ClauseId id : unit_clauses) {
        stack.push_back(id);
    }
    stack.push_back(clauses.size() - 1);

    // Recompute all buckets from new trail
    // TODO: remove this, make faster
    snapshots.back().recompute_buckets(clauses, trail, num_vars);
}

void ExactWatchlist::update_with_new_equation(const Equation& eqn, const Trail& trail) {
    std::size_t first_set = eqn.first_set();
    if (first_set == num_vars) {
        return;
    }
    WatchedId head = snapshots.back().bucket_head[first_set];
    snapshots.back().bucket_head[first_set] = WatchedId();

    while (head.has_value()) {
        WatchedId next = snapshots.back().bucket_next[head];

        // Reduce equations[head] with eqn
        trail.span().reduce(snapshots.back().equations[head]);
        std::size_t new_first_set = snapshots.back().equations[head].first_set();

        // Reclassify equations[head]
        if (new_first_set == bit::vector<>::npos) {
            stack.push_back(head.clauseId());
            snapshots.back().add_to_bucket(head, num_vars);
        } else {
            snapshots.back().add_to_bucket(head, new_first_set);
        }

        head = next;
    }
}

void ExactWatchlist::update_with_decision(const Equation& reduced_eqn, const Trail& trail) {
    // snapshots.push_back(snapshots.back());
    update_with_new_equation(reduced_eqn, trail);
}

void ExactWatchlist::update_with_new_watch(ClauseId id, EquationId /*old_watch*/, WatchType wt, const Trail& trail) {
    // Find new bucket
    EquationId new_watch = 0;
    switch (wt) {
        case WatchType::Zero:
            new_watch = clauses[id].watched0;
            break;
        case WatchType::One:
            new_watch = clauses[id].watched1;
            break;
        case WatchType::Sum:
            // Should never happen
            assert(false && "WatchType::Sum not expected in ExactWatchlist");
            return;
    }
    working_eqn.unsafe_copy(clauses[id].row(new_watch));
    trail.span().reduce(working_eqn);
    std::size_t new_first_set = working_eqn.first_set();

    // Remove watch from current linked list and add to new linked list
    snapshots.back().move_to_bucket(WatchedId(id, wt), new_first_set);

    // Do the same with sum
    if (clauses[id].rows() > 1) {
        working_eqn.unsafe_copy(clauses[id].row(clauses[id].watched0));
        working_eqn += clauses[id].row(clauses[id].watched1);
        trail.span().reduce(working_eqn);
        new_first_set = working_eqn.first_set();
        snapshots.back().move_to_bucket(WatchedId(id, WatchType::Sum), new_first_set);
    }
}

} // namespace heuristics
