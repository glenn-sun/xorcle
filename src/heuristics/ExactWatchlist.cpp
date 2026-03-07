#include "heuristics/ExactWatchlist.h"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "bit/vector.h"

namespace heuristics {

namespace {
inline std::size_t row_index(WatchRowId row) { return static_cast<std::size_t>(row); }
inline WatchRowId row_from_index(std::size_t idx) { return WatchRowId{idx}; }

const char* watch_name(WatchType wt) {
    switch (wt) {
        case WatchType::Zero:
            return "Zero";
        case WatchType::One:
            return "One";
        case WatchType::Sum:
            return "Sum";
    }
    return "?";
}
} // namespace

ExactWatchlist::ExactWatchlist(std::size_t num_vars)
    : ClauseSelector(),
      num_vars(num_vars),
      current_level(0),
      current_trail_ptr(nullptr),
      cells_by_row(),
      row_is_unit_placeholder(),
      row_version(),
      row_next_in_bucket(),
      row_prev_in_bucket(),
      row_bucket(),
      bucket_head_row(num_vars + 2, invalid_row_idx),
      work_stack(),
      unit_clauses(),
      scratch0(num_vars),
      scratch1(num_vars),
      scratch2(num_vars) {
    clear_active_buckets();
}

ExactWatchlist::~ExactWatchlist() = default;

[[noreturn]] void ExactWatchlist::throw_semantic_error(const char* where, const std::string& msg) const {
    std::ostringstream out;
    out << "ExactWatchlist semantic error in " << where << ": " << msg;
    throw std::logic_error(out.str());
}

void ExactWatchlist::ensure_row_storage(WatchRowId row) {
    ensure_row_storage_up_to(static_cast<std::size_t>(row));
}

void ExactWatchlist::ensure_row_storage_up_to(std::size_t max_idx) {
    if (cells_by_row.size() <= max_idx) {
        std::size_t old_size = cells_by_row.size();
        cells_by_row.resize(max_idx + 1);
        row_is_unit_placeholder.resize(max_idx + 1, 0);
        row_version.resize(max_idx + 1, 0);
        row_next_in_bucket.resize(max_idx + 1, invalid_row_idx);
        row_prev_in_bucket.resize(max_idx + 1, invalid_row_idx);
        row_bucket.resize(max_idx + 1, invalid_bucket);

        for (std::size_t i = old_size; i <= max_idx; ++i) {
            cells_by_row[i].reserve(8);
        }
    }
}

bool ExactWatchlist::row_is_placeholder(WatchRowId row) const {
    return row_is_unit_placeholder[row_index(row)] != 0;
}

bool ExactWatchlist::cell_is_valid(WatchRowId row, std::size_t level) const {
    std::size_t idx = static_cast<std::size_t>(row);
    if (idx >= cells_by_row.size()) {
        return false;
    }
    if (level >= cells_by_row[idx].size()) {
        return false;
    }
    return cells_by_row[idx][level].valid;
}

void ExactWatchlist::build_base_row_expression(WatchRowId row, Equation& out) const {
    const Clause& clause = clauses[row.clause_id()];
    assert(clause.rows() > 1);

    switch (row.watch_type()) {
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

ExactWatchlist::ReducedCell* ExactWatchlist::get_or_build_cell(WatchRowId row,
                                                               std::size_t level,
                                                               const Trail& trail) {
    ensure_row_storage(row);
    std::size_t idx = row_index(row);
    assert(idx < cells_by_row.size());

    if (row_is_placeholder(row)) {
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

    ReducedCell& cell = cells_by_row[idx][level];
    if (cell.valid) {
        return &cell;
    }

    assert(level < trail.size());
    const Clause& clause = clauses[row.clause_id()];
    if (cell.eqn.size() != clause.cols()) {
        cell.eqn.resize(clause.cols());
    }
    if (level > 0) {
        ReducedCell& prev_cell = cells_by_row[idx][level - 1];
        if (prev_cell.valid) {
            cell.eqn.unsafe_copy(prev_cell.eqn);
        } else {
            build_base_row_expression(row, cell.eqn);
        }
    } else {
        build_base_row_expression(row, cell.eqn);
    }
    (void)trail[level].span.partial_reduce(cell.eqn);
    cell.valid = true;
    return &cell;
}

void ExactWatchlist::invalidate_row_cache(WatchRowId row) {
    ensure_row_storage(row);
    for (ReducedCell& cell : cells_by_row[static_cast<std::size_t>(row)]) {
        cell.valid = false;
    }
}

std::size_t ExactWatchlist::recompute_watched_row_now(WatchRowId row, const Trail& trail) {
    set_current_trail(trail);
    remove_row_from_active_bucket(row);
    invalidate_row_cache(row);
    ++row_version[row_index(row)];
    ReducedCell* cell = get_or_build_cell(row, trail.size() - 1, trail);
    if (cell == nullptr) {
        return invalid_bucket;
    }
    std::size_t bucket = classify_bucket(cell->eqn);
    insert_row_into_active_bucket(row, bucket);
    return bucket;
}

std::size_t ExactWatchlist::classify_bucket_from_first_set(std::size_t first_set) const {
    if (first_set == bit::vector<>::npos) {
        return zero_bucket();
    }
    if (first_set == num_vars) {
        return const_bucket();
    }
    assert(first_set < num_vars);
    return first_set;
}

std::size_t ExactWatchlist::classify_bucket(const Equation& eqn) const {
    return classify_bucket_from_first_set(eqn.first_set());
}

void ExactWatchlist::clear_active_buckets() {
    std::fill(bucket_head_row.begin(), bucket_head_row.end(), invalid_row_idx);
    std::fill(row_next_in_bucket.begin(), row_next_in_bucket.end(), invalid_row_idx);
    std::fill(row_prev_in_bucket.begin(), row_prev_in_bucket.end(), invalid_row_idx);
    std::fill(row_bucket.begin(), row_bucket.end(), invalid_bucket);
}

void ExactWatchlist::remove_row_from_active_bucket(WatchRowId row) {
    std::size_t idx = row_index(row);
    if (idx >= row_bucket.size() || row_bucket[idx] == invalid_bucket) {
        return;
    }
    std::size_t bucket = row_bucket[idx];

    if (row_prev_in_bucket[idx] != invalid_row_idx) {
        row_next_in_bucket[row_prev_in_bucket[idx]] = row_next_in_bucket[idx];
    } else {
        bucket_head_row[bucket] = row_next_in_bucket[idx];
    }

    if (row_next_in_bucket[idx] != invalid_row_idx) {
        row_prev_in_bucket[row_next_in_bucket[idx]] = row_prev_in_bucket[idx];
    }

    row_next_in_bucket[idx] = invalid_row_idx;
    row_prev_in_bucket[idx] = invalid_row_idx;
    row_bucket[idx] = invalid_bucket;
}

void ExactWatchlist::insert_row_into_active_bucket(WatchRowId row, std::size_t bucket) {
    std::size_t idx = row_index(row);
    assert(idx < row_bucket.size());
    assert(bucket < bucket_count());

    if (row_bucket[idx] != invalid_bucket) {
        remove_row_from_active_bucket(row);
    }

    std::size_t old_head = bucket_head_row[bucket];
    if (old_head != invalid_row_idx) {
        row_prev_in_bucket[old_head] = idx;
    }
    row_next_in_bucket[idx] = old_head;
    row_prev_in_bucket[idx] = invalid_row_idx;
    row_bucket[idx] = bucket;
    bucket_head_row[bucket] = idx;
}

void ExactWatchlist::invalidate_levels_above(std::size_t level) {
    for (auto& row_cells : cells_by_row) {
        for (std::size_t i = level + 1; i < row_cells.size(); ++i) {
            row_cells[i].valid = false;
        }
    }
}

void ExactWatchlist::rebuild_active_buckets_from_valid_cells(const Trail& trail) {
    set_current_trail(trail);
    current_level = trail.size() - 1;
    invalidate_levels_above(current_level);
    clear_active_buckets();

    for (std::size_t i = 0; i < cells_by_row.size(); ++i) {
        WatchRowId row{i};
        if (row_is_placeholder(row)) {
            continue;
        }
        if (!cell_is_valid(row, current_level)) {
            continue;
        }
        const ReducedCell& cell = cells_by_row[i][current_level];
        insert_row_into_active_bucket(row, classify_bucket(cell.eqn));
    }
}

std::vector<ExactWatchlist::VersionedRowRef> ExactWatchlist::detach_bucket_rows(std::size_t bucket) {
    assert(bucket < bucket_count());
    std::vector<VersionedRowRef> out;
    std::size_t idx = bucket_head_row[bucket];
    bucket_head_row[bucket] = invalid_row_idx;
    while (idx != invalid_row_idx) {
        std::size_t next = row_next_in_bucket[idx];
        row_next_in_bucket[idx] = invalid_row_idx;
        row_prev_in_bucket[idx] = invalid_row_idx;
        row_bucket[idx] = invalid_bucket;
        out.push_back(VersionedRowRef{idx, row_version[idx]});
        idx = next;
    }
    return out;
}

std::vector<ExactWatchlist::VersionedRowRef> ExactWatchlist::snapshot_zero_bucket_rows() const {
    std::vector<VersionedRowRef> out;
    for (std::size_t idx = bucket_head_row[zero_bucket()]; idx != invalid_row_idx; idx = row_next_in_bucket[idx]) {
        out.push_back(VersionedRowRef{idx, row_version[idx]});
    }
    return out;
}

std::vector<ExactWatchlist::VersionedRowRef> ExactWatchlist::collect_rows_missing_current_level_cell() const {
    std::vector<VersionedRowRef> out;
    for (std::size_t i = 0; i < cells_by_row.size(); ++i) {
        WatchRowId row{i};
        if (row_is_placeholder(row)) {
            continue;
        }
        if (!cell_is_valid(row, current_level)) {
            out.push_back(VersionedRowRef{i, row_version[i]});
        }
    }
    return out;
}

void ExactWatchlist::push_row_task(WatchRowId row) {
    ensure_row_storage(row);
    push_row_task(VersionedRowRef{row_index(row), row_version[row_index(row)]});
}

void ExactWatchlist::push_row_task(VersionedRowRef ref) {
    work_stack.push_back(DeferredTask{DeferredTaskKind::row_reclassify, ref, 0});
}

void ExactWatchlist::push_rows_as_tasks(std::vector<VersionedRowRef>&& rows) {
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        push_row_task(*it);
    }
}

void ExactWatchlist::push_unit_task(ClauseId id) {
    work_stack.push_back(DeferredTask{DeferredTaskKind::unit_probe, VersionedRowRef{0, 0}, id});
}

bool ExactWatchlist::row_ref_is_stale(const VersionedRowRef& ref) const {
    if (ref.row_idx >= row_version.size()) {
        return true;
    }
    return row_version[ref.row_idx] != ref.version;
}

void ExactWatchlist::append_clause_rows(Clause clause, bool queue_unit_probe) {
    ClauseId id = clauses.size();
    const bool is_unit = clause.rows() <= 1;
    clauses.push_back(std::move(clause));
    ensure_row_storage_up_to(3 * id + 2);

    for (WatchType wt : {WatchType::Zero, WatchType::One, WatchType::Sum}) {
        WatchRowId row{id, wt};
        std::size_t idx = row_index(row);
        row_is_unit_placeholder[idx] = static_cast<std::uint8_t>(is_unit);
        row_version[idx] = 0;
        cells_by_row[idx].clear();
        row_next_in_bucket[idx] = invalid_row_idx;
        row_prev_in_bucket[idx] = invalid_row_idx;
        row_bucket[idx] = invalid_bucket;
    }

    if (is_unit) {
        unit_clauses.push_back(id);
        if (queue_unit_probe) {
            push_unit_task(id);
        }
    }
}

void ExactWatchlist::apply_incremental_level_change(const Trail& trail) {
    set_current_trail(trail);
    std::size_t new_level = trail.size() - 1;
    if (new_level == current_level) {
        return;
    }

    if (new_level != current_level + 1) {
        std::ostringstream msg;
        msg << "expected level " << (current_level + 1) << ", got " << new_level;
        throw_semantic_error("apply_incremental_level_change", msg.str());
    }
    current_level = new_level;
}

ExactWatchlist::ReducedClauseState ExactWatchlist::reduce_clause_watches_now(ClauseId id, const Trail* trail_or_null) {
    assert(id < clauses.size());
    const Clause& clause = clauses[id];
    ReducedClauseState state;
    state.unit = (clause.rows() <= 1);
    if (state.unit) {
        scratch0.unsafe_copy(clause.row(0));
        if (trail_or_null != nullptr) {
            trail_or_null->span().reduce(scratch0);
        }
        state.watch0_zero = scratch0.none();
        state.watch0_one = (scratch0.first_set() == num_vars);
        return state;
    }

    scratch0.unsafe_copy(clause.row(clause.watched0));
    scratch1.unsafe_copy(clause.row(clause.watched1));
    if (trail_or_null != nullptr) {
        trail_or_null->span().reduce(scratch0);
        trail_or_null->span().reduce(scratch1);
    }
    scratch2.unsafe_copy(scratch0);
    scratch2 += scratch1;

    state.watch0_zero = scratch0.none();
    state.watch1_zero = scratch1.none();
    state.sum_zero = scratch2.none();
    state.watch0_one = (scratch0.first_set() == num_vars);
    state.watch1_one = (scratch1.first_set() == num_vars);
    state.sum_one = (scratch2.first_set() == num_vars);
    return state;
}

bool ExactWatchlist::trigger_is_zero(WatchType wt, const ReducedClauseState& state) const {
    switch (wt) {
        case WatchType::Zero:
            return state.watch0_zero;
        case WatchType::One:
            return state.watch1_zero;
        case WatchType::Sum:
            return state.sum_zero;
    }
    return false;
}

bool ExactWatchlist::trigger_allows_action(ClauseId id, WatchType wt, const ReducedClauseState& state) const {
    if (!trigger_is_zero(wt, state)) {
        return false;
    }

    switch (wt) {
        case WatchType::Zero:
            if (state.watch1_one != state.sum_one) {
                std::ostringstream msg;
                msg << "clause " << id << " Zero trigger mismatch: watch1_one=" << state.watch1_one
                    << " sum_one=" << state.sum_one;
                throw_semantic_error("trigger_allows_action", msg.str());
            }
            return !state.watch1_one;
        case WatchType::One:
            if (state.watch0_one != state.sum_one) {
                std::ostringstream msg;
                msg << "clause " << id << " One trigger mismatch: watch0_one=" << state.watch0_one
                    << " sum_one=" << state.sum_one;
                throw_semantic_error("trigger_allows_action", msg.str());
            }
            return !state.watch0_one;
        case WatchType::Sum:
            if (state.watch0_one != state.watch1_one) {
                std::ostringstream msg;
                msg << "clause " << id << " Sum trigger mismatch: watch0_one=" << state.watch0_one
                    << " watch1_one=" << state.watch1_one;
                throw_semantic_error("trigger_allows_action", msg.str());
            }
            return !state.watch1_one;
    }
    return false;
}

std::optional<ClauseSelection> ExactWatchlist::try_select_from_trigger(ClauseId id,
                                                                       WatchType wt,
                                                                       const Trail* trail_or_null,
                                                                       bool require_zero_now) {
    ReducedClauseState state = reduce_clause_watches_now(id, trail_or_null);
    if (state.unit) {
        std::ostringstream msg;
        msg << "trigger selection requested for unit clause " << id;
        throw_semantic_error("try_select_from_trigger", msg.str());
    }

    const bool zero = trigger_is_zero(wt, state);
    if (require_zero_now && !zero) {
        std::ostringstream msg;
        msg << "clause " << id << " trigger " << watch_name(wt) << " expected 0=0, but is not";
        throw_semantic_error("try_select_from_trigger", msg.str());
    }
    if (!zero) {
        return std::nullopt;
    }
    if (!trigger_allows_action(id, wt, state)) {
        return std::nullopt;
    }

    ClauseSelection selection{id, wt};
    validate_selection_or_throw(selection, trail_or_null);
    return selection;
}

std::optional<ClauseSelection> ExactWatchlist::try_select_unit_clause(ClauseId id, const Trail* trail_or_null) {
    ReducedClauseState state = reduce_clause_watches_now(id, trail_or_null);
    if (!state.unit) {
        std::ostringstream msg;
        msg << "unit clause probe requested for non-unit clause " << id;
        throw_semantic_error("try_select_unit_clause", msg.str());
    }
    if (state.watch0_one) {
        return std::nullopt;
    }
    ClauseSelection selection{id, std::nullopt};
    validate_selection_or_throw(selection, trail_or_null);
    return selection;
}

void ExactWatchlist::validate_selection_or_throw(const ClauseSelection& selection, const Trail* trail_or_null) {
    if (selection.id >= clauses.size()) {
        std::ostringstream msg;
        msg << "selected clause " << selection.id << " out of range";
        throw_semantic_error("validate_selection_or_throw", msg.str());
    }

    ReducedClauseState state = reduce_clause_watches_now(selection.id, trail_or_null);
    if (state.unit) {
        if (selection.zero_watch_hint.has_value()) {
            std::ostringstream msg;
            msg << "unit clause " << selection.id << " returned non-null hint";
            throw_semantic_error("validate_selection_or_throw", msg.str());
        }
        if (state.watch0_one) {
            std::ostringstream msg;
            msg << "unit clause " << selection.id << " selected but reduced to 0=1";
            throw_semantic_error("validate_selection_or_throw", msg.str());
        }
        return;
    }

    if (!selection.zero_watch_hint.has_value()) {
        std::ostringstream msg;
        msg << "non-unit clause " << selection.id << " selected without hint";
        throw_semantic_error("validate_selection_or_throw", msg.str());
    }
    if (!trigger_allows_action(selection.id, *selection.zero_watch_hint, state)) {
        std::ostringstream msg;
        msg << "non-unit clause " << selection.id << " selected with invalid hint "
            << watch_name(*selection.zero_watch_hint);
        throw_semantic_error("validate_selection_or_throw", msg.str());
    }
}

std::optional<ClauseSelection> ExactWatchlist::process_one_deferred_task() {
    if (work_stack.empty()) {
        return std::nullopt;
    }

    DeferredTask task = work_stack.back();
    work_stack.pop_back();

    switch (task.kind) {
        case DeferredTaskKind::row_reclassify: {
            if (row_ref_is_stale(task.row_ref)) {
                return std::nullopt;
            }
            if (current_trail_ptr == nullptr) {
                throw_semantic_error("process_one_deferred_task", "missing trail during row reclassification");
            }
            WatchRowId row = row_from_index(task.row_ref.row_idx);
            ReducedCell* cell = get_or_build_cell(row, current_level, *current_trail_ptr);
            if (cell == nullptr) {
                return std::nullopt;
            }
            std::size_t first_set = current_trail_ptr->span().partial_reduce(cell->eqn);
            std::size_t new_bucket = classify_bucket_from_first_set(first_set);
            insert_row_into_active_bucket(row, new_bucket);
            if (new_bucket != zero_bucket()) {
                return std::nullopt;
            }
            return try_select_from_trigger(row.clause_id(), row.watch_type(), current_trail_ptr, true);
        }
        case DeferredTaskKind::unit_probe:
            return try_select_unit_clause(task.clause_id, current_trail_ptr);
    }
    throw_semantic_error("process_one_deferred_task", "unknown deferred task kind");
}

std::optional<ClauseId> ExactWatchlist::next() {
    auto maybe = next_with_hint();
    if (!maybe.has_value()) {
        return std::nullopt;
    }
    return maybe->id;
}

std::optional<ClauseSelection> ExactWatchlist::next_with_hint() {
    while (!work_stack.empty()) {
        auto maybe = process_one_deferred_task();
        if (maybe.has_value()) {
            validate_selection_or_throw(*maybe, current_trail_ptr);
            return maybe;
        }
    }

#ifndef NDEBUG
    if (current_trail_ptr != nullptr) {
        debug_assert_no_actionable_clause_when_idle(*current_trail_ptr);
    }
#endif
    return std::nullopt;
}

void ExactWatchlist::add_starting_clause(Clause clause) {
    append_clause_rows(std::move(clause), true);

    const Clause& stored_clause = clauses.back();

    if (stored_clause.rows() <= 1) {
        return;
    }

    ClauseId id = clauses.size() - 1;
    for (WatchType wt : {WatchType::Zero, WatchType::One, WatchType::Sum}) {
        WatchRowId row{id, wt};
        ensure_row_storage(row);
        if (cells_by_row[row].empty()) {
            cells_by_row[row].resize(1);
            cells_by_row[row][0].eqn.resize(num_vars + 1);
        }
        ReducedCell& cell = cells_by_row[row][0];
        if (cell.eqn.size() != stored_clause.cols()) {
            cell.eqn.resize(stored_clause.cols());
        }
        build_base_row_expression(row, cell.eqn);
        cell.valid = true;
        std::size_t bucket = classify_bucket(cell.eqn);
        insert_row_into_active_bucket(row, bucket);
        if (bucket == zero_bucket()) {
            push_row_task(row);
        }
    }
}

void ExactWatchlist::add_asserting_clause(Clause clause,
                                          const Trail& trail,
                                          std::size_t /*jumpable_levels*/) {
    set_current_trail(trail);
    append_clause_rows(std::move(clause), false);
    ClauseId asserting_id = clauses.size() - 1;

    work_stack.clear();
    rebuild_active_buckets_from_valid_cells(trail);

    std::vector<VersionedRowRef> missing_rows = collect_rows_missing_current_level_cell();
    std::erase_if(missing_rows, [&](const VersionedRowRef& ref) {
        return row_from_index(ref.row_idx).clause_id() == asserting_id;
    });
    std::vector<VersionedRowRef> zero_rows = snapshot_zero_bucket_rows();

    // Push bottom-to-top so asserting rows run first.
    push_rows_as_tasks(std::move(missing_rows));
    push_rows_as_tasks(std::move(zero_rows));
    for (ClauseId id : unit_clauses) {
        push_unit_task(id);
    }

    if (clauses[asserting_id].rows() > 1) {
        std::vector<VersionedRowRef> rows;
        rows.reserve(3);
        for (WatchType wt : {WatchType::Zero, WatchType::One, WatchType::Sum}) {
            WatchRowId row{asserting_id, wt};
            rows.push_back(VersionedRowRef{row_index(row), row_version[row_index(row)]});
        }
        push_rows_as_tasks(std::move(rows));
    }

#ifndef NDEBUG
    debug_assert_active_buckets_match_cells(trail);
#endif
}

void ExactWatchlist::update_with_new_equation(const Equation& eqn, const Trail& trail) {
    set_current_trail(trail);
    apply_incremental_level_change(trail);

    std::size_t first_set = eqn.first_set();
    if (first_set == bit::vector<>::npos) {
        return;
    }
    std::size_t src_bucket = (first_set == num_vars) ? const_bucket() : first_set;
    if (src_bucket >= bucket_count()) {
        std::ostringstream msg;
        msg << "invalid source bucket " << src_bucket << " from first_set " << first_set;
        throw_semantic_error("update_with_new_equation", msg.str());
    }

    std::vector<VersionedRowRef> detached_rows = detach_bucket_rows(src_bucket);
    push_rows_as_tasks(std::move(detached_rows));

#ifndef NDEBUG
    if (work_stack.empty()) {
        debug_assert_active_buckets_match_cells(trail);
    }
#endif
}

void ExactWatchlist::update_with_decision(const Equation& reduced_eqn, const Trail& trail) {
    set_current_trail(trail);
    if (!work_stack.empty()) {
        std::ostringstream msg;
        msg << "decision arrived while " << work_stack.size() << " deferred tasks are still pending";
        throw_semantic_error("update_with_decision", msg.str());
    }
    update_with_new_equation(reduced_eqn, trail);
}

void ExactWatchlist::update_with_new_watch(ClauseId id,
                                           EquationId /*old_watch*/,
                                           WatchType wt,
                                           const Trail& trail) {
    set_current_trail(trail);
    apply_incremental_level_change(trail);

    switch (wt) {
        case WatchType::Zero:
        case WatchType::One: {
            std::size_t bucket = recompute_watched_row_now(WatchRowId{id, wt}, trail);
            if (bucket == zero_bucket()) {
                push_row_task(WatchRowId{id, wt});
            }
            break;
        }
        case WatchType::Sum:
            throw_semantic_error("update_with_new_watch", "direct Sum watch updates are not expected");
    }

    if (clauses[id].rows() > 1) {
        std::size_t bucket = recompute_watched_row_now(WatchRowId{id, WatchType::Sum}, trail);
        if (bucket == zero_bucket()) {
            push_row_task(WatchRowId{id, WatchType::Sum});
        }
    }

#ifndef NDEBUG
    if (work_stack.empty()) {
        debug_assert_active_buckets_match_cells(trail);
    }
#endif
}

#ifndef NDEBUG
void ExactWatchlist::debug_assert_active_buckets_match_cells(const Trail& trail) const {
    std::size_t level = trail.size() - 1;
    for (std::size_t i = 0; i < cells_by_row.size(); ++i) {
        WatchRowId row{i};
        if (row_is_unit_placeholder[i]) {
            assert(row_bucket[i] == invalid_bucket);
            continue;
        }
        if (!cell_is_valid(row, level)) {
            continue;
        }
        const ReducedCell& cell = cells_by_row[i][level];
        std::size_t expected_bucket = classify_bucket(cell.eqn);
        assert(row_bucket[i] == expected_bucket);
    }
}

void ExactWatchlist::debug_assert_no_actionable_clause_when_idle(const Trail& trail) {
    std::vector<std::uint8_t> seen_clause(clauses.size(), 0);
    for (std::size_t idx = bucket_head_row[zero_bucket()]; idx != invalid_row_idx; idx = row_next_in_bucket[idx]) {
        WatchRowId row = row_from_index(idx);
        if (row_is_placeholder(row)) {
            continue;
        }
        if (row.clause_id() >= clauses.size() || seen_clause[row.clause_id()]) {
            continue;
        }
        seen_clause[row.clause_id()] = 1;

        ReducedClauseState state = reduce_clause_watches_now(row.clause_id(), &trail);
        const bool actionable = trigger_allows_action(row.clause_id(), WatchType::Zero, state)
            || trigger_allows_action(row.clause_id(), WatchType::One, state)
            || trigger_allows_action(row.clause_id(), WatchType::Sum, state);
        assert(!actionable);
    }

    for (ClauseId id : unit_clauses) {
        auto maybe = try_select_unit_clause(id, &trail);
        assert(!maybe.has_value());
    }
}
#endif

} // namespace heuristics
