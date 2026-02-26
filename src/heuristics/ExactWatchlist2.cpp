#include "heuristics/ExactWatchlist2.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iostream>

#include "bit/vector.h"

namespace heuristics {

namespace {
inline std::size_t wid_idx(EW2WatchedId wid) { return static_cast<std::size_t>(wid); }
inline EW2WatchedId wid_from_idx(std::size_t idx) { return EW2WatchedId{idx}; }

bool ew2_debug_queue_drop_logging_enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("PARITY_DEBUG_EW2_QUEUE_DROPS");
        if (v == nullptr) return false;
        return !(v[0] == '\0' || (v[0] == '0' && v[1] == '\0'));
    }();
    return enabled;
}
} // namespace

ExactWatchlist2::ExactWatchlist2(std::size_t num_vars, ExactWatchlist2Config config)
    : ClauseSelector(),
      num_vars(num_vars),
      config(config),
      current_trail_ptr(nullptr),
      cells_by_row(),
      row_structural_placeholder(),
      row_work_version(),
      active_next_idx(),
      active_prev_idx(),
      active_bucket(),
      bucket_head_idx(num_vars + 2, invalid_wid_idx),
      pending(),
      pending_unit_set(),
      pending_watch_set(),
      deferred_work(),
      unit_clauses(),
      working_eqn(num_vars),
      working_eqn2(num_vars) {
    clear_active_buckets();
}

ExactWatchlist2::~ExactWatchlist2() {
    if (!ew2_debug_queue_drop_logging_enabled()) {
        return;
    }
    double pct = 0.0;
    if (debug_pending_enqueued != 0) {
        pct = 100.0 * static_cast<double>(debug_pending_discarded_on_conflict_clear)
            / static_cast<double>(debug_pending_enqueued);
    }
    std::cerr << "[EW2 queue debug] enqueued=" << debug_pending_enqueued
              << " popped=" << debug_pending_popped
              << " discarded_before_propagation_due_conflict=" << debug_pending_discarded_on_conflict_clear
              << " conflict_clears=" << debug_conflict_clears
              << " deferred_bucket_scans=" << debug_deferred_bucket_scans_created
              << " deferred_reseed_scans=" << debug_deferred_reseed_scans_created
              << " deferred_rows_processed=" << debug_deferred_rows_processed
              << " deferred_rows_skipped_stale=" << debug_deferred_rows_skipped_stale
              << " deferred_rows_skipped_not_zero=" << debug_deferred_rows_skipped_not_zero
              << " discarded_pct_of_enqueued=" << pct << "%\n";
}

WatchType ExactWatchlist2::normalize_nonunit_hint(WatchType wt) const {
    switch (wt) {
        case WatchType::Zero:
            return WatchType::Zero;
        case WatchType::One:
        case WatchType::Sum:
            return WatchType::One;
    }
    assert(false && "unreachable WatchType");
    return WatchType::One;
}

std::size_t ExactWatchlist2::watch_pending_key(ClauseId id, WatchType wt) const {
    WatchType norm = normalize_nonunit_hint(wt);
    assert(norm == WatchType::Zero || norm == WatchType::One);
    return 2 * id + (norm == WatchType::One ? 1 : 0);
}

void ExactWatchlist2::push_pending_unit(ClauseId id) {
    if (id >= pending_unit_set.size()) {
        pending_unit_set.resize(id + 1, 0);
    }
    if (pending_unit_set[id]) {
        return;
    }
    pending_unit_set[id] = 1;
    pending.push_back(ClauseSelection{id, std::nullopt});
    ++debug_pending_enqueued;
}

void ExactWatchlist2::push_pending_watch(ClauseId id, WatchType wt) {
    assert(id < clauses.size());
    assert(clauses[id].rows() > 1);
    WatchType norm = normalize_nonunit_hint(wt);
    std::size_t key = watch_pending_key(id, norm);
    if (key >= pending_watch_set.size()) {
        pending_watch_set.resize(key + 1, 0);
    }
    if (pending_watch_set[key]) {
        return;
    }
    pending_watch_set[key] = 1;
    pending.push_back(ClauseSelection{id, norm});
    ++debug_pending_enqueued;
}

void ExactWatchlist2::push_pending_from_zero_row(EW2WatchedId wid) {
    ClauseId id = wid.clauseId();
    if (clauses[id].rows() <= 1) {
        push_pending_unit(id);
        return;
    }
    push_pending_watch(id, wid.watchType());
}

ClauseSelection ExactWatchlist2::pop_pending() {
    ClauseSelection out;
    if (config.discipline == PendingDiscipline::fifo_queue) {
        out = pending.front();
        pending.pop_front();
    } else {
        out = pending.back();
        pending.pop_back();
    }
    ++debug_pending_popped;

    if (out.zero_watch_hint.has_value()) {
        assert(*out.zero_watch_hint == WatchType::Zero || *out.zero_watch_hint == WatchType::One);
        std::size_t key = watch_pending_key(out.id, *out.zero_watch_hint);
        assert(key < pending_watch_set.size());
        pending_watch_set[key] = 0;
        assert(out.id < clauses.size());
        assert(clauses[out.id].rows() > 1);
    } else {
        assert(out.id < clauses.size());
        assert(clauses[out.id].rows() <= 1);
        assert(out.id < pending_unit_set.size());
        pending_unit_set[out.id] = 0;
    }
    return out;
}

void ExactWatchlist2::push_deferred_bucket_scan(std::vector<DeferredRowRef>&& rows) {
    if (rows.empty()) {
        return;
    }
    DeferredWorkItem item{DeferredWorkKind::bucket_reassign, std::move(rows), 0};
    if (config.discipline == PendingDiscipline::fifo_queue) {
        deferred_work.push_back(std::move(item));
    } else {
        deferred_work.push_back(std::move(item));
    }
    ++debug_deferred_bucket_scans_created;
}

void ExactWatchlist2::push_deferred_zero_reseed(std::vector<DeferredRowRef>&& rows) {
    if (rows.empty()) {
        return;
    }
    DeferredWorkItem item{DeferredWorkKind::zero_reseed, std::move(rows), 0};
    if (config.discipline == PendingDiscipline::fifo_queue) {
        deferred_work.push_back(std::move(item));
    } else {
        deferred_work.push_back(std::move(item));
    }
    ++debug_deferred_reseed_scans_created;
}

void ExactWatchlist2::ensure_row_capacity(EW2WatchedId wid) {
    ensure_row_capacity_up_to(static_cast<std::size_t>(wid));
}

void ExactWatchlist2::ensure_row_capacity_up_to(std::size_t max_idx) {
    if (cells_by_row.size() <= max_idx) {
        std::size_t old_size = cells_by_row.size();
        cells_by_row.resize(max_idx + 1);
        row_structural_placeholder.resize(max_idx + 1, 0);
        row_work_version.resize(max_idx + 1, 0);
        active_next_idx.resize(max_idx + 1, invalid_wid_idx);
        active_prev_idx.resize(max_idx + 1, invalid_wid_idx);
        active_bucket.resize(max_idx + 1, invalid_bucket);

        // A small reserve amortizes cache-column growth without much footprint.
        for (std::size_t i = old_size; i <= max_idx; ++i) {
            cells_by_row[i].reserve(8);
        }
    }
}

bool ExactWatchlist2::row_is_placeholder(EW2WatchedId wid) const {
    return row_structural_placeholder[wid_idx(wid)] != 0;
}

bool ExactWatchlist2::cell_is_valid(EW2WatchedId wid, std::size_t level) const {
    std::size_t idx = static_cast<std::size_t>(wid);
    if (idx >= cells_by_row.size()) {
        return false;
    }
    if (level >= cells_by_row[idx].size()) {
        return false;
    }
    return cells_by_row[idx][level].valid;
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
    return ensure_cell_existing_row(wid, level, trail);
}

ExactWatchlist2::CacheCell* ExactWatchlist2::ensure_cell_existing_row(EW2WatchedId wid,
                                                                      std::size_t level,
                                                                      const Trail& trail) {
    std::size_t idx = wid_idx(wid);
    assert(idx < cells_by_row.size());

    if (row_is_placeholder(wid)) {
        return nullptr;
    }

    if (cells_by_row[idx].size() <= level) {
        std::size_t old_size = cells_by_row[idx].size();
        cells_by_row[idx].resize(level + 1);
        for (std::size_t i = old_size; i <= level; ++i) {
            cells_by_row[idx][i].eqn.resize(num_vars + 1);
            cells_by_row[idx][i].valid = false;
        }
    }

    CacheCell& cell = cells_by_row[idx][level];
    if (cell.valid) {
        return &cell;
    }

    assert(level < trail.size());
    const Clause& clause = clauses[wid.clauseId()];
    if (cell.eqn.size() != clause.cols()) {
        cell.eqn.resize(clause.cols());
    }
    build_base_expression(wid, cell.eqn);
    (void)trail[level].span.partial_reduce(cell.eqn);
    cell.valid = true;
    return &cell;
}

std::size_t ExactWatchlist2::classify_bucket_from_first_set(std::size_t f) const {
    if (f == bit::vector<>::npos) {
        return zero_bucket();
    }
    if (f == num_vars) {
        return const_bucket();
    }
    assert(f < num_vars);
    return f;
}

std::size_t ExactWatchlist2::classify_bucket(const Equation& eqn) const {
    return classify_bucket_from_first_set(eqn.first_set());
}

void ExactWatchlist2::clear_active_buckets() {
    std::fill(bucket_head_idx.begin(), bucket_head_idx.end(), invalid_wid_idx);
    std::fill(active_next_idx.begin(), active_next_idx.end(), invalid_wid_idx);
    std::fill(active_prev_idx.begin(), active_prev_idx.end(), invalid_wid_idx);
    std::fill(active_bucket.begin(), active_bucket.end(), invalid_bucket);
}

void ExactWatchlist2::insert_active(EW2WatchedId wid, std::size_t bucket) {
    std::size_t idx = wid_idx(wid);
    assert(idx < active_bucket.size());
    assert(bucket < num_buckets());

    if (active_bucket[idx] != invalid_bucket) {
        remove_active(wid);
    }

    std::size_t old_head_idx = bucket_head_idx[bucket];
    if (old_head_idx != invalid_wid_idx) {
        active_prev_idx[old_head_idx] = idx;
    }
    active_next_idx[idx] = old_head_idx;
    active_prev_idx[idx] = invalid_wid_idx;
    active_bucket[idx] = bucket;
    bucket_head_idx[bucket] = idx;
}

void ExactWatchlist2::remove_active(EW2WatchedId wid) {
    std::size_t idx = wid_idx(wid);
    if (idx >= active_bucket.size() || active_bucket[idx] == invalid_bucket) {
        return;
    }
    std::size_t bucket = active_bucket[idx];

    if (active_prev_idx[idx] != invalid_wid_idx) {
        active_next_idx[active_prev_idx[idx]] = active_next_idx[idx];
    } else {
        bucket_head_idx[bucket] = active_next_idx[idx];
    }

    if (active_next_idx[idx] != invalid_wid_idx) {
        active_prev_idx[active_next_idx[idx]] = active_prev_idx[idx];
    }

    active_next_idx[idx] = invalid_wid_idx;
    active_prev_idx[idx] = invalid_wid_idx;
    active_bucket[idx] = invalid_bucket;
}

void ExactWatchlist2::sync_current_level_rebuild_only(const Trail& trail) {
    set_current_trail(trail);
    current_level = trail.size() - 1;
    discard_levels_after(current_level);
    clear_active_buckets();
    // Materialize the current column and rebuild active buckets in one pass.
    for (std::size_t i = 0; i < cells_by_row.size(); ++i) {
        EW2WatchedId wid{i};
        CacheCell* cell = ensure_cell_existing_row(wid, current_level, trail);
        if (cell == nullptr) {
            continue;
        }
        insert_active(wid, classify_bucket(cell->eqn));
    }
#if defined(PARITY_ENABLE_EXPENSIVE_SELECTOR_CHECKS)
    if (deferred_work.empty()) {
        assert_active_membership_matches_current_level(trail);
    }
#endif
}

void ExactWatchlist2::discard_levels_after(std::size_t level) {
    for (auto& row : cells_by_row) {
        for (std::size_t i = level + 1; i < row.size(); ++i) {
            row[i].valid = false;
        }
    }
}

void ExactWatchlist2::seed_pending_from_zero_bucket() {
    // Conservative backjump/restart seeding is now lazy: snapshot the current zero bucket
    // and let next_with_hint() materialize ready clause tasks on demand.
    push_deferred_zero_reseed(snapshot_zero_bucket_rows());
}

void ExactWatchlist2::on_level_change_incremental(const Trail& trail) {
    set_current_trail(trail);
    std::size_t new_level = trail.size() - 1;
    if (new_level == current_level) {
        return;
    }

    // Forward search can only increase by one decision level. Backjumps/restarts call
    // sync_current_level_rebuild_only() from add_asserting_clause().
    assert(new_level == current_level + 1);
    current_level = new_level;
}

std::vector<ExactWatchlist2::DeferredRowRef> ExactWatchlist2::detach_bucket_to_snapshot(std::size_t bucket) {
    assert(bucket < num_buckets());
    std::vector<DeferredRowRef> out;
    std::size_t idx = bucket_head_idx[bucket];
    bucket_head_idx[bucket] = invalid_wid_idx;
    while (idx != invalid_wid_idx) {
        std::size_t next_idx = active_next_idx[idx];
        active_next_idx[idx] = invalid_wid_idx;
        active_prev_idx[idx] = invalid_wid_idx;
        active_bucket[idx] = invalid_bucket;
        out.push_back(DeferredRowRef{idx, row_work_version[idx]});
        idx = next_idx;
    }
    return out;
}

std::vector<ExactWatchlist2::DeferredRowRef> ExactWatchlist2::snapshot_zero_bucket_rows() const {
    std::vector<DeferredRowRef> out;
    for (std::size_t idx = bucket_head_idx[zero_bucket()]; idx != invalid_wid_idx; idx = active_next_idx[idx]) {
        out.push_back(DeferredRowRef{idx, row_work_version[idx]});
    }
    return out;
}

bool ExactWatchlist2::step_one_deferred_work() {
    if (deferred_work.empty()) {
        return false;
    }
    assert(current_trail_ptr != nullptr);
    const Trail& trail = *current_trail_ptr;
    const bool fifo = (config.discipline == PendingDiscipline::fifo_queue);
    DeferredWorkItem& item = fifo ? deferred_work.front() : deferred_work.back();

    auto process_row_ref = [&](const DeferredRowRef& ref, bool zero_reseed_mode) {
        ++debug_deferred_rows_processed;
        if (ref.row_idx >= cells_by_row.size() || ref.row_idx >= row_work_version.size()) {
            return;
        }
        if (row_work_version[ref.row_idx] != ref.version) {
            ++debug_deferred_rows_skipped_stale;
            return;
        }
        if (zero_reseed_mode) {
            if (active_bucket[ref.row_idx] != zero_bucket()) {
                ++debug_deferred_rows_skipped_not_zero;
                return;
            }
            push_pending_from_zero_row(wid_from_idx(ref.row_idx));
            return;
        }

        EW2WatchedId wid = wid_from_idx(ref.row_idx);
        CacheCell* cell = ensure_cell_existing_row(wid, current_level, trail);
        if (cell == nullptr) {
            return;
        }
        std::size_t f = trail.span().partial_reduce(cell->eqn);
        std::size_t new_bucket = classify_bucket_from_first_set(f);
        insert_active(wid, new_bucket);
        if (new_bucket == zero_bucket()) {
            push_pending_from_zero_row(wid);
        }
    };
    switch (item.kind) {
        case DeferredWorkKind::bucket_reassign:
            if (item.pos < item.rows.size()) {
                process_row_ref(item.rows[item.pos], false);
                ++item.pos;
            }
            break;
        case DeferredWorkKind::zero_reseed:
            if (item.pos < item.rows.size()) {
                process_row_ref(item.rows[item.pos], true);
                ++item.pos;
            }
            break;
        default:
            assert(false && "unreachable DeferredWorkKind");
            break;
    }

    if (item.pos >= item.rows.size()) {
        if (fifo) {
            deferred_work.pop_front();
        } else {
            deferred_work.pop_back();
        }
    }
    return true;
}

void ExactWatchlist2::invalidate_row(EW2WatchedId wid) {
    ensure_row_capacity(wid);
    for (CacheCell& cell : cells_by_row[static_cast<std::size_t>(wid)]) {
        cell.valid = false;
    }
}

void ExactWatchlist2::refresh_watched_row_now(EW2WatchedId wid, const Trail& trail) {
    set_current_trail(trail);
    remove_active(wid);
    invalidate_row(wid);
    ++row_work_version[wid_idx(wid)];
    CacheCell* cell = ensure_cell_existing_row(wid, trail.size() - 1, trail);
    if (cell == nullptr) {
        return;
    }
    std::size_t bucket = classify_bucket(cell->eqn);
    insert_active(wid, bucket);
    if (bucket == zero_bucket()) {
        push_pending_from_zero_row(wid);
    }
}

void ExactWatchlist2::enqueue_clause_watch_tasks_now(ClauseId id, const Trail& trail) {
    assert(id < clauses.size());
    const Clause& clause = clauses[id];
    if (clause.rows() <= 1) {
        push_pending_unit(id);
        return;
    }

    working_eqn.unsafe_copy(clause.row(clause.watched0));
    working_eqn2.unsafe_copy(clause.row(clause.watched1));
    trail.span().reduce(working_eqn);
    trail.span().reduce(working_eqn2);

    if (working_eqn.none()) {
        push_pending_watch(id, WatchType::Zero);
    }
    if (working_eqn2.none() || working_eqn == working_eqn2) {
        push_pending_watch(id, WatchType::One);
    }
}

void ExactWatchlist2::append_clause_rows(const Clause& clause, bool enqueue_if_unit) {
    ClauseId id = clauses.size();
    clauses.push_back(clause);
    ensure_row_capacity_up_to(3 * id + 2);

    for (WatchType wt : {WatchType::Zero, WatchType::One, WatchType::Sum}) {
        EW2WatchedId wid{id, wt};
        std::size_t idx = wid_idx(wid);
        row_structural_placeholder[idx] = static_cast<std::uint8_t>(clause.rows() <= 1);
        row_work_version[idx] = 0;
        cells_by_row[idx].clear();
        active_next_idx[idx] = invalid_wid_idx;
        active_prev_idx[idx] = invalid_wid_idx;
        active_bucket[idx] = invalid_bucket;
    }

    if (pending_unit_set.size() <= id) {
        pending_unit_set.resize(id + 1, 0);
    }
    if (pending_watch_set.size() < 2 * (id + 1)) {
        pending_watch_set.resize(2 * (id + 1), 0);
    }

    if (clause.rows() <= 1) {
        unit_clauses.push_back(id);
        if (enqueue_if_unit) {
            push_pending_unit(id);
        }
    }
}

std::optional<ClauseId> ExactWatchlist2::next() {
    auto sel = next_with_hint();
    if (!sel.has_value()) {
        return std::nullopt;
    }
    return sel->id;
}

std::optional<ClauseSelection> ExactWatchlist2::next_with_hint() {
    while (pending.empty()) {
        if (!step_one_deferred_work()) {
            return std::nullopt;
        }
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
                cells_by_row[wid][0].eqn.resize(num_vars + 1);
            }
            CacheCell& cell = cells_by_row[wid][0];
            if (cell.eqn.size() != clause.cols()) {
                cell.eqn.resize(clause.cols());
            }
            build_base_expression(wid, cell.eqn);
            cell.valid = true;
            insert_active(wid, classify_bucket(cell.eqn));
        }
    }
}

void ExactWatchlist2::add_asserting_clause(const Clause& clause, const Trail& trail, std::size_t /*jumpable_levels*/) {
    set_current_trail(trail);
    append_clause_rows(clause, false);
    ClauseId asserting_id = clauses.size() - 1;

    std::size_t dropped_now = pending.size();
    debug_pending_discarded_on_conflict_clear += dropped_now;
    ++debug_conflict_clears;
    if (ew2_debug_queue_drop_logging_enabled() && dropped_now > 0) {
        double pct = 0.0;
        if (debug_pending_enqueued != 0) {
            pct = 100.0 * static_cast<double>(debug_pending_discarded_on_conflict_clear)
                / static_cast<double>(debug_pending_enqueued);
        }
        std::cerr << "[EW2 queue debug] conflict clear dropped " << dropped_now
                  << " pending entries before propagation; cumulative dropped="
                  << debug_pending_discarded_on_conflict_clear << "/" << debug_pending_enqueued
                  << " (" << pct << "%)\n";
    }
    pending.clear();
    deferred_work.clear();
    std::fill(pending_unit_set.begin(), pending_unit_set.end(), 0);
    std::fill(pending_watch_set.begin(), pending_watch_set.end(), 0);
    for (ClauseId id : unit_clauses) {
        push_pending_unit(id);
    }

    sync_current_level_rebuild_only(trail);
    seed_pending_from_zero_bucket();
    enqueue_clause_watch_tasks_now(asserting_id, trail);
}

void ExactWatchlist2::update_with_new_equation(const Equation& eqn, const Trail& trail) {
    set_current_trail(trail);
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

    push_deferred_bucket_scan(detach_bucket_to_snapshot(src_bucket));

#if defined(PARITY_ENABLE_EXPENSIVE_SELECTOR_CHECKS)
    if (deferred_work.empty()) {
        assert_active_membership_matches_current_level(trail);
    }
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
    set_current_trail(trail);
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
    if (deferred_work.empty()) {
        assert_active_membership_matches_current_level(trail);
    }
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
