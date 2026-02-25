#include "heuristics/ExactWatchlist2.h"

#include <algorithm>
#include <cassert>

#include "bit/vector.h"

namespace heuristics {

ExactWatchlist2::ExactWatchlist2(std::size_t num_vars, ExactWatchlist2Config config)
    : ClauseSelector(),
      num_vars(num_vars),
      config(config),
      level_stamp_by_index(1, 1),
      next_level_stamp(2),
      cells_by_row(),
      row_epoch(),
      row_structural_placeholder(),
      active_next(),
      active_prev(),
      active_bucket(),
      bucket_head(num_vars + 2),
      pending(),
      pending_set(),
      unit_clauses(),
      working_eqn(num_vars + 1) {
    clear_active_buckets();
}

void ExactWatchlist2::push_pending(ClauseId id) {
    if (dedup_enabled()) {
        if (id >= pending_set.size()) {
            pending_set.resize(id + 1, false);
        }
        if (pending_set[id]) {
            return;
        }
        pending_set[id] = true;
    }
    pending.push_back(id);
}

ClauseId ExactWatchlist2::pop_pending() {
    ClauseId id;
    if (config.discipline == PendingDiscipline::fifo_queue) {
        id = pending.front();
        pending.pop_front();
    } else {
        id = pending.back();
        pending.pop_back();
    }

    if (dedup_enabled()) {
        assert(id < pending_set.size());
        pending_set[id] = false;
    }
    return id;
}

void ExactWatchlist2::ensure_row_capacity(EW2WatchedId wid) {
    std::size_t idx = static_cast<std::size_t>(wid);
    if (cells_by_row.size() <= idx) {
        cells_by_row.resize(idx + 1);
        row_epoch.resize(idx + 1, 0);
        row_structural_placeholder.resize(idx + 1, false);
        active_next.resize(idx + 1);
        active_prev.resize(idx + 1);
        active_bucket.resize(idx + 1, invalid_bucket);
    }
}

bool ExactWatchlist2::row_is_placeholder(EW2WatchedId wid) const {
    return row_structural_placeholder[static_cast<std::size_t>(wid)];
}

bool ExactWatchlist2::cell_is_valid(EW2WatchedId wid, std::size_t level) const {
    std::size_t idx = static_cast<std::size_t>(wid);
    if (idx >= cells_by_row.size()) {
        return false;
    }
    if (level >= cells_by_row[idx].size()) {
        return false;
    }
    const CacheCell& cell = cells_by_row[idx][level];
    return cell.computed &&
           cell.epoch == row_epoch[idx] &&
           level < level_stamp_by_index.size() &&
           cell.level_stamp == level_stamp_by_index[level];
}

void ExactWatchlist2::partial_reduce_in_place(Equation& eqn, const algebra::RowSpace& span) const {
    while (true) {
        std::size_t f = eqn.first_set();
        if (f == bit::vector<>::npos) {
            return;
        }
        if (!span.get_pivot_cols().element(f)) {
            return;
        }
        eqn ^= span.row(span.get_col_to_row(f));
    }
}

void ExactWatchlist2::build_base_expression(EW2WatchedId wid, Equation& out) const {
    const Clause& clause = clauses[wid.clauseId()];
    assert(clause.rows() > 1);

    switch (wid.watchType()) {
        case WatchType::Zero:
            out.unsafe_copy(clause.row(clause.watched0));
            return;
        case WatchType::One:
            out.unsafe_copy(clause.row(clause.watched1));
            return;
        case WatchType::Sum:
            out.unsafe_copy(clause.row(clause.watched0));
            out += clause.row(clause.watched1);
            return;
    }
    assert(false && "unreachable WatchType");
}

ExactWatchlist2::CacheCell* ExactWatchlist2::ensure_cell(EW2WatchedId wid, std::size_t level, const Trail& trail) {
    ensure_row_capacity(wid);
    std::size_t idx = static_cast<std::size_t>(wid);

    if (row_is_placeholder(wid)) {
        return nullptr;
    }

    if (cells_by_row[idx].size() <= level) {
        cells_by_row[idx].resize(level + 1);
    }

    CacheCell& cell = cells_by_row[idx][level];
    if (cell.computed &&
        cell.epoch == row_epoch[idx] &&
        level < level_stamp_by_index.size() &&
        cell.level_stamp == level_stamp_by_index[level]) {
        return &cell;
    }

    assert(level < trail.size());
    const Clause& clause = clauses[wid.clauseId()];
    if (cell.eqn.size() != clause.cols()) {
        cell.eqn = Equation(clause.cols() - 1);
    }
    build_base_expression(wid, cell.eqn);
    partial_reduce_in_place(cell.eqn, trail[level].span);
    cell.epoch = row_epoch[idx];
    cell.level_stamp = level_stamp_by_index[level];
    cell.computed = true;
    return &cell;
}

std::size_t ExactWatchlist2::classify_bucket(const Equation& eqn) const {
    std::size_t f = eqn.first_set();
    if (f == bit::vector<>::npos) {
        return zero_bucket();
    }
    if (f == num_vars) {
        return const_bucket();
    }
    assert(f < num_vars);
    return f;
}

void ExactWatchlist2::clear_active_buckets() {
    std::fill(bucket_head.begin(), bucket_head.end(), EW2WatchedId());
    std::fill(active_next.begin(), active_next.end(), EW2WatchedId());
    std::fill(active_prev.begin(), active_prev.end(), EW2WatchedId());
    std::fill(active_bucket.begin(), active_bucket.end(), invalid_bucket);
}

void ExactWatchlist2::insert_active(EW2WatchedId wid, std::size_t bucket) {
    std::size_t idx = static_cast<std::size_t>(wid);
    assert(idx < active_bucket.size());
    assert(bucket < num_buckets());

    if (active_bucket[idx] != invalid_bucket) {
        remove_active(wid);
    }

    EW2WatchedId old_head = bucket_head[bucket];
    if (old_head.has_value()) {
        active_prev[old_head] = wid;
    }
    active_next[idx] = old_head;
    active_prev[idx] = EW2WatchedId();
    active_bucket[idx] = bucket;
    bucket_head[bucket] = wid;
}

void ExactWatchlist2::remove_active(EW2WatchedId wid) {
    std::size_t idx = static_cast<std::size_t>(wid);
    if (idx >= active_bucket.size() || active_bucket[idx] == invalid_bucket) {
        return;
    }
    std::size_t bucket = active_bucket[idx];

    if (active_prev[idx].has_value()) {
        active_next[active_prev[idx]] = active_next[idx];
    } else {
        bucket_head[bucket] = active_next[idx];
    }

    if (active_next[idx].has_value()) {
        active_prev[active_next[idx]] = active_prev[idx];
    }

    active_next[idx] = EW2WatchedId();
    active_prev[idx] = EW2WatchedId();
    active_bucket[idx] = invalid_bucket;
}

void ExactWatchlist2::rebuild_active_from_level(const Trail& trail) {
    clear_active_buckets();
    current_level = trail.size() - 1;

    for (std::size_t i = 0; i < cells_by_row.size(); ++i) {
        EW2WatchedId wid{i};
        CacheCell* cell = ensure_cell(wid, current_level, trail);
        if (cell == nullptr) {
            continue;
        }
        insert_active(wid, classify_bucket(cell->eqn));
    }
}

void ExactWatchlist2::sync_current_level_rebuild_only(const Trail& trail) {
    current_level = trail.size() - 1;
    if (level_stamp_by_index.size() <= current_level) {
        level_stamp_by_index.resize(current_level + 1, 0);
    }
    level_stamp_by_index[current_level] = next_level_stamp++;
    // Materialize the current column first, then rebuild active buckets.
    for (std::size_t i = 0; i < cells_by_row.size(); ++i) {
        EW2WatchedId wid{i};
        (void)ensure_cell(wid, current_level, trail);
    }
    rebuild_active_from_level(trail);
#if defined(PARITY_ENABLE_EXPENSIVE_SELECTOR_CHECKS)
    assert_active_membership_matches_current_level(trail);
#endif
}

void ExactWatchlist2::seed_pending_from_zero_bucket() {
    // Conservative backjump/restart seeding: any currently-zero watched expression may need
    // reevaluation under the restored trail, even if it was already examined deeper.
    for (EW2WatchedId wid = bucket_head[zero_bucket()]; wid.has_value(); wid = active_next[wid]) {
        push_pending(wid.clauseId());
    }
}

void ExactWatchlist2::on_level_change_incremental(const Trail& trail) {
    std::size_t new_level = trail.size() - 1;
    if (new_level == current_level) {
        return;
    }

    // Forward search can only increase by one decision level. Backjumps/restarts call
    // sync_current_level_rebuild_only() from add_asserting_clause().
    assert(new_level == current_level + 1);
    current_level = new_level;
    if (level_stamp_by_index.size() <= current_level) {
        level_stamp_by_index.resize(current_level + 1, 0);
    }
    level_stamp_by_index[current_level] = next_level_stamp++;
}

void ExactWatchlist2::invalidate_row(EW2WatchedId wid) {
    ensure_row_capacity(wid);
    std::size_t idx = static_cast<std::size_t>(wid);
    ++row_epoch[idx];
}

void ExactWatchlist2::refresh_watched_row_now(EW2WatchedId wid, const Trail& trail) {
    remove_active(wid);
    invalidate_row(wid);
    CacheCell* cell = ensure_cell(wid, trail.size() - 1, trail);
    if (cell == nullptr) {
        return;
    }
    std::size_t bucket = classify_bucket(cell->eqn);
    insert_active(wid, bucket);
    if (bucket == zero_bucket()) {
        push_pending(wid.clauseId());
    }
}

void ExactWatchlist2::append_clause_rows(const Clause& clause, bool enqueue_if_unit) {
    ClauseId id = clauses.size();
    clauses.push_back(clause);

    for (WatchType wt : {WatchType::Zero, WatchType::One, WatchType::Sum}) {
        EW2WatchedId wid{id, wt};
        ensure_row_capacity(wid);
        std::size_t idx = static_cast<std::size_t>(wid);
        row_structural_placeholder[idx] = (clause.rows() <= 1);
        row_epoch[idx] = 0;
        cells_by_row[idx].clear();
        active_next[idx] = EW2WatchedId();
        active_prev[idx] = EW2WatchedId();
        active_bucket[idx] = invalid_bucket;
    }

    if (pending_set.size() <= id) {
        pending_set.resize(id + 1, false);
    }

    if (clause.rows() <= 1) {
        unit_clauses.push_back(id);
        if (enqueue_if_unit) {
            push_pending(id);
        }
    }
}

std::optional<ClauseId> ExactWatchlist2::next() {
    if (pending.empty()) {
        return std::nullopt;
    }
    return pop_pending();
}

void ExactWatchlist2::add_starting_clause(const Clause& clause) {
    append_clause_rows(clause, true);

    // Initial clauses are added before search starts, at level 0 with empty trail span.
    if (clause.rows() > 1) {
        ClauseId id = clauses.size() - 1;
        for (WatchType wt : {WatchType::Zero, WatchType::One, WatchType::Sum}) {
            EW2WatchedId wid{id, wt};
            ensure_row_capacity(wid);
            if (cells_by_row[wid].empty()) {
                cells_by_row[wid].resize(1);
            }
            CacheCell& cell = cells_by_row[wid][0];
            if (cell.eqn.size() != clause.cols()) {
                cell.eqn = Equation(clause.cols() - 1);
            }
            build_base_expression(wid, cell.eqn);
            cell.epoch = row_epoch[wid];
            cell.level_stamp = level_stamp_by_index[0];
            cell.computed = true;
            insert_active(wid, classify_bucket(cell.eqn));
        }
    }
}

void ExactWatchlist2::add_asserting_clause(const Clause& clause, const Trail& trail, std::size_t /*jumpable_levels*/) {
    append_clause_rows(clause, false);

    pending.clear();
    if (dedup_enabled()) {
        std::fill(pending_set.begin(), pending_set.end(), false);
    }
    for (ClauseId id : unit_clauses) {
        push_pending(id);
    }
    push_pending(clauses.size() - 1);

    sync_current_level_rebuild_only(trail);
    seed_pending_from_zero_bucket();
}

void ExactWatchlist2::update_with_new_equation(const Equation& eqn, const Trail& trail) {
    on_level_change_incremental(trail);

    std::size_t f = eqn.first_set();
    if (f == bit::vector<>::npos) {
        return;
    }

    std::size_t src_bucket;
    if (f == num_vars) {
        src_bucket = const_bucket();
    } else {
        assert(f < num_vars);
        src_bucket = f;
    }

    EW2WatchedId head = bucket_head[src_bucket];
    bucket_head[src_bucket] = EW2WatchedId();

    while (head.has_value()) {
        std::size_t head_idx = static_cast<std::size_t>(head);
        EW2WatchedId next = active_next[head_idx];

        active_next[head_idx] = EW2WatchedId();
        active_prev[head_idx] = EW2WatchedId();
        active_bucket[head_idx] = invalid_bucket;

        CacheCell* cell = ensure_cell(head, current_level, trail);
        if (cell != nullptr) {
            partial_reduce_in_place(cell->eqn, trail.span());
            std::size_t new_bucket = classify_bucket(cell->eqn);
            insert_active(head, new_bucket);
            if (new_bucket == zero_bucket()) {
                push_pending(head.clauseId());
            }
        }

        head = next;
    }

#if defined(PARITY_ENABLE_EXPENSIVE_SELECTOR_CHECKS)
    assert_active_membership_matches_current_level(trail);
#endif
}

void ExactWatchlist2::update_with_decision(const Equation& reduced_eqn, const Trail& trail) {
    // Decisions use the same incremental pivot-bucket update as propagations.
    // Backjumps/restarts remain conservative (rebuild + zero-bucket reseed) in
    // add_asserting_clause() until we track enough state to narrow that reseed.
    update_with_new_equation(reduced_eqn, trail);
}

void ExactWatchlist2::update_with_new_watch(ClauseId id,
                                            EquationId /*old_watch*/,
                                            WatchType wt,
                                            const Trail& trail) {
    on_level_change_incremental(trail);

    switch (wt) {
        case WatchType::Zero:
        case WatchType::One:
            refresh_watched_row_now(EW2WatchedId{id, wt}, trail);
            break;
        case WatchType::Sum:
            assert(false && "WatchType::Sum not expected in ExactWatchlist2");
            return;
    }

    if (clauses[id].rows() > 1) {
        refresh_watched_row_now(EW2WatchedId{id, WatchType::Sum}, trail);
    }

#if defined(PARITY_ENABLE_EXPENSIVE_SELECTOR_CHECKS)
    assert_active_membership_matches_current_level(trail);
#endif
}

#if defined(PARITY_ENABLE_EXPENSIVE_SELECTOR_CHECKS)
void ExactWatchlist2::assert_active_membership_matches_current_level(const Trail& trail) const {
    std::size_t level = trail.size() - 1;
    for (std::size_t i = 0; i < cells_by_row.size(); ++i) {
        EW2WatchedId wid{i};
        if (row_structural_placeholder[i]) {
            assert(active_bucket[i] == invalid_bucket);
            continue;
        }
        if (!cell_is_valid(wid, level)) {
            continue;
        }
        const CacheCell& cell = cells_by_row[i][level];
        std::size_t want_bucket = classify_bucket(cell.eqn);
        assert(active_bucket[i] == want_bucket);
    }
}

#endif

} // namespace heuristics
