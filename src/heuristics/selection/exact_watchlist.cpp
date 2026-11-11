#include "heuristics/selection/exact_watchlist.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>

#include "algebra/reduce_result.h"
#include "algebra/sparse/sparse_gf2_vector.h"

namespace xorcle {

namespace {

constexpr std::size_t npos = static_cast<std::size_t>(-1);

} // namespace

void ExactWatchlistCells<DenseGF2Vector>::relabel(const CompactionTransform& transform,
                                                  std::size_t new_num_cols) {
    const std::size_t old_num_words = num_words_;
    const std::size_t old_constant_col_idx = num_cols_ - 1;
    num_cols_ = new_num_cols;
    num_words_ = (new_num_cols + word_mask) >> word_shift;
    const auto to_new = [&](const std::size_t col_idx) {
        return col_idx == old_constant_col_idx ? new_num_cols - 1 : transform.to_new(col_idx);
    };

    // The words below the first dropped column stay as they are.
    std::size_t num_unmoved_words = 0;
    while (num_unmoved_words < num_words_ &&
           ((num_unmoved_words + 1) << word_shift) <= old_constant_col_idx &&
           transform.to_new(((num_unmoved_words + 1) << word_shift) - 1) ==
               ((num_unmoved_words + 1) << word_shift) - 1) {
        ++num_unmoved_words;
    }
    // No column moves up, so a cell is relabelled in place: each new word lies at
    // or below the old word being read, and is zeroed once that one has been.
    const std::size_t num_cells = words_.size() / old_num_words;
    for (std::size_t cell = 0; cell < num_cells; ++cell) {
        const Word* const old_words = words_.data() + cell * old_num_words;
        Word* const words = words_.data() + cell * num_words_;
        std::memmove(words, old_words, num_unmoved_words * sizeof(Word));
        std::size_t num_written_words = num_unmoved_words;
        std::size_t w = num_unmoved_words;
        while (w < old_num_words) {
            if (w + 4 <= old_num_words &&
                (old_words[w] | old_words[w + 1] | old_words[w + 2] | old_words[w + 3]) == 0) {
                w += 4;
                continue;
            }
            for (Word word = old_words[w]; word != 0; word &= word - 1) {
                const std::size_t new_col_idx =
                    to_new((w << word_shift) + static_cast<std::size_t>(std::countr_zero(word)));
                // A free pair's cells may still carry a dropped column.
                if (new_col_idx == CompactionTransform::npos) {
                    continue;
                }
                const std::size_t new_w = new_col_idx >> word_shift;
                while (num_written_words <= new_w) {
                    words[num_written_words++] = 0;
                }
                words[new_w] |= Word{1} << (new_col_idx & word_mask);
            }
            ++w;
        }
        std::fill(words + num_written_words, words + num_words_, Word{0});
    }
    words_.resize(num_cells * num_words_);
}

template <GF2Vector V>
ExactWatchlistTemplate<V>::ExactWatchlistTemplate(std::size_t num_vars,
                                                  const ClauseDatabase& database)
    : ClauseSelector(database) {
    reset(num_vars + 1);
}

template <GF2Vector V>
void ExactWatchlistTemplate<V>::reset(std::size_t num_cols) {
    num_cols_ = num_cols;
    cells_.reset(num_cols);
    key_.clear();
    undo_.clear();
    in_active_clause_.clear();
    pair_clause_.clear();
    pair_is_unit_.clear();
    clause_pair_.assign(database_.num_clauses(), no_pair);
    buckets_.assign(num_cols, {});
    bucket_entries_ = 0;
    cleanup_threshold_ = num_cols;
    pivot_row_.assign(num_cols, nullptr);
    pivot_col_idxs_.clear();
    level_pivot_mark_.assign(1, 0);
    paused_head_ = 0;
    paused_end_ = 0;
    saves_.clear();
    level_save_mark_.assign(1, 0);
    level_cells_save_mark_.assign(1, 0);
    level_stamp_.assign(1, 0);
    restored_cells_.clear();
    clear_side_queue();
}

template <GF2Vector V>
void ExactWatchlistTemplate<V>::open_level() {
    level_pivot_mark_.push_back(pivot_col_idxs_.size());
    level_save_mark_.push_back(saves_.size());
    level_cells_save_mark_.push_back(cells_.save_mark());
    level_stamp_.push_back(next_stamp_++);
}

template <GF2Vector V>
inline std::uint32_t ExactWatchlistTemplate<V>::reduce_by_span(Row* cell, std::size_t lead) const {
    while (lead != npos) {
        const Row* row = pivot_row_[lead];
        if (row == nullptr) {
            break;
        }
        lead = cells_.add_and_next(cell, row, lead);
    }
    return lead == npos ? zero_key : static_cast<std::uint32_t>(lead);
}

template <GF2Vector V>
inline std::uint32_t ExactWatchlistTemplate<V>::reduce_by_span_and_first(Row* cell, std::size_t lead,
                                                       const Row* first,
                                                       std::uint32_t first_key) const {
    while (lead != npos) {
        if (const Row* row = pivot_row_[lead]; row != nullptr) {
            lead = cells_.add_and_next(cell, row, lead);
        } else if (lead == first_key) {
            lead = cells_.add_and_next(cell, first, lead);
        } else {
            break;
        }
    }
    return lead == npos ? zero_key : static_cast<std::uint32_t>(lead);
}

template <GF2Vector V>
inline auto ExactWatchlistTemplate<V>::watch_row(std::uint32_t cell) const -> const Row* {
    const std::uint32_t pair = cell >> 1;
    const ClauseId id = pair_clause_[pair];
    const ClauseWatches& watches = database_.records()[id].watches;
    const Clause& clause = database_.clauses()[id];
    const std::size_t row_idx =
        (cell & 1) != 0 ? watches.watched1 : (pair_is_unit_[pair] != 0 ? 0 : watches.watched0);
    return Cells::row_of(gf2_vector_of<V>(clause.row(row_idx)));
}

template <GF2Vector V>
inline void ExactWatchlistTemplate<V>::build_first(std::uint32_t cell, const Row* watch) {
    Row* bits = cells_.cell(cell);
    cells_.copy(bits, watch);
    key_[cell] = reduce_by_span(bits, cells_.leading_bit(bits));
}

template <GF2Vector V>
inline void ExactWatchlistTemplate<V>::build_second(std::uint32_t cell, const Row* watch) {
    Row* bits = cells_.cell(cell);
    cells_.copy(bits, watch);
    key_[cell] = reduce_by_span_and_first(bits, cells_.leading_bit(bits), cells_.cell(cell - 1),
                                          key_[cell - 1]);
}

template <GF2Vector V>
inline void ExactWatchlistTemplate<V>::take_reduction(std::uint32_t cell, const Equation& reduced,
                                           const ReduceResult& result) {
    cells_.copy(cells_.cell(cell), Cells::row_of(gf2_vector_of<V>(reduced)));
    switch (result.kind) {
        case ReduceResult::Kind::eq_true:
            key_[cell] = zero_key;
            break;
        case ReduceResult::Kind::eq_false:
            key_[cell] = constant_key();
            break;
        case ReduceResult::Kind::leading:
            key_[cell] = static_cast<std::uint32_t>(result.bit);
            break;
    }
}

template <GF2Vector V>
void ExactWatchlistTemplate<V>::build_pair(ClauseId id) {
    const Clause& clause = database_.clauses()[id];
    assert(clause.repr() == V::repr && "a clause is in another representation than the cells");
    assert(clause.cols() == num_cols_ && "a clause is over another numbering than the trail");
    assert(clause.rows() >= 1 && "a clause has no rows");

    const bool unit = clause.rows() == 1;
    std::uint32_t pair = 0;
    if (!free_pairs_.empty()) {
        pair = free_pairs_.back();
        free_pairs_.pop_back();
        pair_clause_[pair] = static_cast<std::uint32_t>(id);
        pair_is_unit_[pair] = unit ? 1 : 0;
        for (const std::uint32_t cell : {2 * pair, 2 * pair + 1}) {
            assert(undo_[cell].save_stamp == 0 && "a free pair's cell is still saved at a level");
            key_[cell] = zero_key;
            ++undo_[cell].gen;
        }
    } else {
        pair = static_cast<std::uint32_t>(pair_clause_.size());
        pair_clause_.push_back(static_cast<std::uint32_t>(id));
        pair_is_unit_.push_back(unit ? 1 : 0);
        cells_.add_pair();
        for (int k = 0; k < 2; ++k) {
            key_.push_back(zero_key);
            undo_.emplace_back();
            in_active_clause_.push_back(0);
        }
    }
    const std::uint32_t first = 2 * pair;
    clause_pair_[id] = pair;
    in_active_clause_[first] = 1;
    in_active_clause_[first + 1] = 1;

    // Nothing to return to below the current level, so a backjump rebuilds.
    build_first(first, watch_row(first));
    place(first);
    push_rebuild_entry(first, top_level());
    if (!unit) {
        build_second(first + 1, watch_row(first + 1));
        place(first + 1);
        push_rebuild_entry(first + 1, top_level());
    }
    maybe_queue(pair);
}

template <GF2Vector V>
void ExactWatchlistTemplate<V>::rebuild_pair(std::uint32_t pair) {
    assert(top_level() == 0 && pivot_col_idxs_.empty() && "a pair is rebuilt against a trail");
    const std::uint32_t first = 2 * pair;
    const bool unit = database_.clauses()[pair_clause_[pair]].rows() == 1;
    pair_is_unit_[pair] = unit ? 1 : 0;
    for (const std::uint32_t cell : {first, first + 1}) {
        key_[cell] = zero_key;
        ++undo_[cell].gen;
    }
    build_first(first, watch_row(first));
    if (!unit) {
        build_second(first + 1, watch_row(first + 1));
    }
}

template <GF2Vector V>
void ExactWatchlistTemplate<V>::clean_buckets() {
    cleanup_stamp_.resize(key_.size(), 0);
    ++cleanup_epoch_;
    std::size_t kept = 0;
    // Stable, so the entries left are swept in the order they were placed.
    for (std::size_t col_idx = 0; col_idx < buckets_.size(); ++col_idx) {
        std::vector<std::uint32_t>& bucket = buckets_[col_idx];
        std::size_t out = 0;
        for (const std::uint32_t cell : bucket) {
            if (key_[cell] == col_idx && cleanup_stamp_[cell] != cleanup_epoch_ &&
                in_active_clause_[cell] != 0) {
                cleanup_stamp_[cell] = cleanup_epoch_;
                bucket[out++] = cell;
            }
        }
        bucket.resize(out);
        kept += out;
    }
    bucket_entries_ = kept;
    cleanup_threshold_ = 2 * kept + num_cols_ - num_dropped_vars_;
}

template <GF2Vector V>
inline void ExactWatchlistTemplate<V>::save(std::uint32_t cell, std::size_t level) {
    if (level == 0 || undo_[cell].save_stamp == level_stamp_[level]) {
        return;
    }
    const std::uint32_t key = key_[cell];
    const std::size_t offset = cells_.save(cells_.cell(cell), saved_from_bit(key));
    saves_.push_back(SaveEntry{cell, undo_[cell].gen, key, undo_[cell].save_stamp, offset});
    undo_[cell].save_stamp = level_stamp_[level];
}

template <GF2Vector V>
inline void ExactWatchlistTemplate<V>::copy_back(const SaveEntry& entry) {
    cells_.copy_back(cells_.cell(entry.cell), saved_from_bit(entry.key), entry.offset);
    key_[entry.cell] = entry.key;
}

template <GF2Vector V>
inline void ExactWatchlistTemplate<V>::push_rebuild_entry(std::uint32_t cell, std::size_t level) {
    // An entry already at this level rebuilds once `gen` has moved past it.
    if (level == 0 || undo_[cell].save_stamp == level_stamp_[level]) {
        return;
    }
    saves_.push_back(SaveEntry{cell, undo_[cell].gen, key_[cell], undo_[cell].save_stamp, rebuild_entry});
    undo_[cell].save_stamp = level_stamp_[level];
}

template <GF2Vector V>
inline bool ExactWatchlistTemplate<V>::advance(std::uint32_t cell, std::size_t level) {
    save(cell, level);
    Row* bits = cells_.cell(cell);
    const std::uint32_t pair = cell >> 1;

    if ((cell & 1) != 0) {
        const std::uint32_t key = reduce_by_span_and_first(
            bits, key_[cell], cells_.cell(cell - 1), key_[cell - 1]);
        key_[cell] = key;
        place(cell);
        return key == zero_key;
    }

    const std::uint32_t key = reduce_by_span(bits, key_[cell]);
    key_[cell] = key;
    place(cell);
    if (pair_is_unit_[pair] != 0) {
        return key != constant_key();
    }
    // `B` may now lead where `A` does, which `B`'s reduction by `A` forbids.
    const std::uint32_t second = cell + 1;
    bool second_is_zero = false;
    if (key != zero_key && key_[second] == key) {
        save(second, level);
        Row* second_bits = cells_.cell(second);
        const std::size_t second_lead = cells_.add_and_next(second_bits, bits, key);
        key_[second] = reduce_by_span_and_first(second_bits, second_lead, bits, key);
        place(second);
        second_is_zero = key_[second] == zero_key;
    }
    return key == zero_key || second_is_zero;
}

template <GF2Vector V>
std::optional<ClauseSelection> ExactWatchlistTemplate<V>::resume_sweeps() {
    const std::size_t level = top_level();
    while (paused_head_ < paused_end_) {
        PausedSweep& sweep = paused_sweeps_[paused_head_];
        assert(pivot_row_[sweep.col_idx] != nullptr && "a paused sweep's column is not a pivot");
        const std::uint32_t col_idx = sweep.col_idx;
        while (sweep.cursor < sweep.cells.size()) {
            const std::uint32_t cell = sweep.cells[sweep.cursor++];
            if (key_[cell] != col_idx) {
                continue;
            }
            if (in_active_clause_[cell] == 0) {
                continue;
            }
            const std::uint32_t pair = cell >> 1;
            // A pair with a `0 = 1` cell cannot trigger: an `A` of `0 = 1` satisfies the
            // clause, and a `B` of `0 = 1` means the second watch or the sum of both is
            // `0 = 1`, so either watch entering the span satisfies it by the other. So the
            // partner stays in this column's bucket unreduced. Watches move only on a
            // clause that can trigger, so only a backjump changes the `0 = 1` cell, and
            // one below this row leaves the partner a partial reduction in its bucket
            // again.
            if (key_[cell ^ 1] == constant_key()) {
                buckets_[col_idx].push_back(cell);
                ++bucket_entries_;
                continue;
            }
            if (!advance(cell, level)) {
                continue;
            }
            bring_current(pair);
            if (const std::optional<ClauseSelection> selection =
                    selection_of(pair_clause_[pair], trigger(pair))) {
                maybe_clean_buckets();
                return selection;
            }
        }
        sweep.cells.clear();
        if (++paused_head_ == paused_end_) {
            paused_head_ = 0;
            paused_end_ = 0;
        }
        maybe_clean_buckets();
    }
    return std::nullopt;
}

template <GF2Vector V>
void ExactWatchlistTemplate<V>::drop_paused_sweeps() {
    for (std::size_t i = paused_head_; i < paused_end_; ++i) {
        PausedSweep& sweep = paused_sweeps_[i];
        std::vector<std::uint32_t>& bucket = buckets_[sweep.col_idx];
        const auto unreached = sweep.cells.begin() + static_cast<std::ptrdiff_t>(sweep.cursor);
        bucket.insert(bucket.end(), unreached, sweep.cells.end());
        bucket_entries_ += sweep.cells.size() - sweep.cursor;
        sweep.cells.clear();
    }
    paused_head_ = 0;
    paused_end_ = 0;
}

template <GF2Vector V>
void ExactWatchlistTemplate<V>::bring_current(std::uint32_t pair) {
    const std::uint32_t first = 2 * pair;
    const std::uint32_t end = pair_is_unit_[pair] != 0 ? first + 1 : first + 2;
    for (std::uint32_t cell = first; cell < end; ++cell) {
        const std::uint32_t key = key_[cell];
        if (key != zero_key && key != constant_key() && pivot_row_[key] != nullptr) {
            (void)advance(cell, top_level());
        }
    }
}

template <GF2Vector V>
inline auto ExactWatchlistTemplate<V>::trigger(std::uint32_t pair) const -> Trigger {
    const std::uint32_t first_key = key_[2 * pair];
    if (pair_is_unit_[pair] != 0) {
        return first_key == constant_key() ? Trigger::none : Trigger::unit;
    }
    // A watch that is `0 = 1` satisfies the clause.
    if (first_key == constant_key()) {
        return Trigger::none;
    }
    const std::uint32_t second_key = key_[2 * pair + 1];
    if (first_key == zero_key) {
        return second_key == constant_key() ? Trigger::none : Trigger::zero;
    }
    return second_key == zero_key ? Trigger::one : Trigger::none;
}

template <GF2Vector V>
inline std::optional<ClauseSelection> ExactWatchlistTemplate<V>::selection_of(ClauseId id, Trigger trigger) {
    switch (trigger) {
        case Trigger::none:
            return std::nullopt;
        case Trigger::unit:
            return ClauseSelection{id, std::nullopt};
        case Trigger::zero:
            return ClauseSelection{id, WatchType::zero};
        case Trigger::one:
            return ClauseSelection{id, WatchType::one};
    }
    return std::nullopt;
}

template <GF2Vector V>
inline void ExactWatchlistTemplate<V>::maybe_queue(std::uint32_t pair) {
    const ClauseId id = pair_clause_[pair];
    if (in_side_queue_[id] != 0 || trigger(pair) == Trigger::none) {
        return;
    }
    in_side_queue_[id] = 1;
    side_queue_.push_back(id);
}

template <GF2Vector V>
void ExactWatchlistTemplate<V>::clear_side_queue() {
    for (std::size_t i = side_queue_head_; i < side_queue_.size(); ++i) {
        in_side_queue_[side_queue_[i]] = 0;
    }
    side_queue_.clear();
    side_queue_head_ = 0;
}

template <GF2Vector V>
std::optional<ClauseSelection> ExactWatchlistTemplate<V>::next() {
    while (side_queue_head_ < side_queue_.size()) {
        const ClauseId id = side_queue_[side_queue_head_++];
        in_side_queue_[id] = 0;
        if (database_.active_flags()[id] == 0) {
            continue;
        }
        const std::uint32_t pair = clause_pair_[id];
        assert(pair != no_pair && "an active clause has no cells");
        if (trigger(pair) == Trigger::none) {
            continue;
        }
        // A partner a paused sweep has yet to reach may be `0 = 1`, which the hint forbids.
        bring_current(pair);
        if (const std::optional<ClauseSelection> selection = selection_of(id, trigger(pair))) {
            return selection;
        }
    }
    side_queue_.clear();
    side_queue_head_ = 0;
    return resume_sweeps();
}

template <GF2Vector V>
void ExactWatchlistTemplate<V>::update_with_new_clause(const ClauseId id) {
    assert(id == clause_pair_.size() && "a clause was added out of order");
    in_side_queue_.push_back(0);
    clause_pair_.push_back(no_pair);
    build_pair(id);
}

template <GF2Vector V>
void ExactWatchlistTemplate<V>::update_with_inactive_clause(ClauseId id) {
    const std::uint32_t pair = clause_pair_[id];
    if (pair != no_pair) {
        retired_pairs_.push_back(pair);
        in_active_clause_[2 * pair] = 0;
        in_active_clause_[2 * pair + 1] = 0;
    }
}

template <GF2Vector V>
void ExactWatchlistTemplate<V>::update_with_narrowed_clause(ClauseId id) {
    assert(top_level() == 0 && "a clause was narrowed above the root");
    update_with_inactive_clause(id);
    build_pair(id);
}

template <GF2Vector V>
void ExactWatchlistTemplate<V>::update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) {
    const RowSpace& span = trail.span();
    assert(span.repr() == V::repr && "the trail is in another representation than the cells");
    assert(span.cols() == num_cols_ && "the trail is over another numbering than the cells");
    // Every earlier row was reported, so the new one is the last: complete
    // propagation never learns below the top level.
    assert(pivot_col_idxs_.size() + 1 == span.rows() &&
           "an equation was learned out of order, which only a propagation budget does");
    const std::size_t trail_idx = span.rows() - 1;
    const std::size_t level = trail.level();
    assert(trail.level_of(trail_idx) == level && "an equation was learned below the top level");
    assert((level == top_level() || level == top_level() + 1) && "a decision level was skipped");
    if (level > top_level()) {
        assert(paused_head_ == paused_end_ && side_queue_head_ == side_queue_.size() &&
               "a decision was taken before propagation was quiescent");
        open_level();
    }
    const Equation& row = trail.row(trail_idx);
    assert(row == reduced_eqn && "the reported equation is not the row the trail stores");
    (void)reduced_eqn;

    const Row* bits = Cells::row_of(gf2_vector_of<V>(row));
    const std::size_t lead = cells_.leading_bit(bits);
    assert(lead != npos && "a stored trail row is `0 = 0`");
    if (lead == constant_key()) {
        // The conflict: a backjump follows, and nothing leads on the constant.
        return;
    }
    assert(pivot_row_[lead] == nullptr && "a new trail row leads on a pivot");
    pivot_row_[lead] = bits;
    pivot_col_idxs_.push_back(static_cast<std::uint32_t>(lead));

    if (paused_end_ == paused_sweeps_.size()) {
        // Finished slots move behind the queue, for reuse.
        std::rotate(paused_sweeps_.begin(),
                    paused_sweeps_.begin() + static_cast<std::ptrdiff_t>(paused_head_),
                    paused_sweeps_.begin() + static_cast<std::ptrdiff_t>(paused_end_));
        paused_end_ -= paused_head_;
        paused_head_ = 0;
        if (paused_end_ == paused_sweeps_.size()) {
            paused_sweeps_.emplace_back();
        }
    }
    PausedSweep& sweep = paused_sweeps_[paused_end_++];
    sweep.col_idx = static_cast<std::uint32_t>(lead);
    sweep.cursor = 0;
    // The bucket takes the cleared storage, and no build can place into it while the
    // column is a pivot.
    sweep.cells.swap(buckets_[lead]);
    bucket_entries_ -= sweep.cells.size();
    maybe_clean_buckets();
#ifndef NDEBUG
    if (debug_check_due(16)) {
        debug_check_cells(trail);
    }
#endif
}

template <GF2Vector V>
HeldReduction ExactWatchlistTemplate<V>::copy_reduced_companion(ClauseId id, WatchType type,
                                                     Equation& reduced, ReduceResult& result) {
    const std::uint32_t pair = clause_pair_[id];
    assert(pair != no_pair && "an active clause has no cells");
    assert(trigger(pair) == (type == WatchType::zero ? Trigger::zero : Trigger::one) &&
           "the trigger asked about does not hold on the cells");
    const std::uint32_t cell = 2 * pair + (type == WatchType::zero ? 1U : 0U);
    const std::uint32_t key = key_[cell];
    assert(key != constant_key() && (key == zero_key || pivot_row_[key] == nullptr) &&
           "a triggered pair's standing cell is `0 = 1` or was left lagging");
    Cells::copy_out(gf2_vector_of<V>(reduced), cells_.cell(cell));
    result = key == zero_key ? ReduceResult::eq_true() : ReduceResult::leading(key);
    // `B` may hold an `A` added in, every one of them in the span now that `A` is `0 = 0`.
    return type == WatchType::zero ? HeldReduction::congruent : HeldReduction::exact;
}

template <GF2Vector V>
void ExactWatchlistTemplate<V>::update_with_new_watch(ClauseId id, ClauseRowIdx, WatchType type,
                                           const Trail& trail,
                                           const WatchReductions& reductions) {
    assert(database_.is_clause_active(id) && "a watch moved on an inactive clause");
    assert(database_.clauses()[id].rows() >= 2 && "a watch moved on a unit clause");
    assert(trail.level() == top_level() && "a watch moved at a level the selector never saw");
    (void)trail;
    const std::uint32_t pair = clause_pair_[id];
    const std::uint32_t first = 2 * pair;
    const std::size_t level = top_level();
#ifndef NDEBUG
    for (const std::uint32_t cell : {first, first + 1}) {
        const std::uint32_t key = key_[cell];
        assert((key == zero_key || key == constant_key() || pivot_row_[key] == nullptr) &&
               "a watch moved on a pair a sweep left lagging, which cannot trigger");
    }
#endif
    // `B` is reduced by `A`, so a new first watch replaces both.
    if (type == WatchType::zero) {
        ++undo_[first].gen;
        take_reduction(first, *reductions.first, reductions.first_result);
        place(first);
        push_rebuild_entry(first, level);
    }
    ++undo_[first + 1].gen;
    take_reduction(first + 1, *reductions.second, reductions.second_result);
    place(first + 1);
    push_rebuild_entry(first + 1, level);
#ifndef NDEBUG
    debug_assert_cells_are_builds(pair, type == WatchType::zero);
#endif
    maybe_queue(pair);
    maybe_clean_buckets();
}

template <GF2Vector V>
void ExactWatchlistTemplate<V>::restore(std::size_t level) {
    for (std::uint32_t kind = 0; kind < 2; ++kind) {
        restored_by_kind_[kind].clear();
    }
    rebuilt_cells_.clear();
    const auto note = [&](std::uint32_t cell, bool rebuild) {
        restored_by_kind_[cell & 1].push_back((cell & ~std::uint32_t{1}) | (rebuild ? 1U : 0U));
        if (rebuild) {
            rebuilt_cells_.push_back(cell);
        }
    };
    const std::size_t mark = level_save_mark_[level + 1];
    if (level + 1 == top_level()) {
        // A level saves a cell at most once.
        for (std::size_t i = saves_.size(); i-- > mark;) {
            const SaveEntry& entry = saves_[i];
            const std::uint32_t cell = entry.cell;
            undo_[cell].save_stamp = entry.prev_stamp;
            // A saved state from before a watch move is of another watch.
            const bool rebuild = entry.offset == rebuild_entry || entry.gen != undo_[cell].gen;
            if (!rebuild) {
                copy_back(entry);
            }
            note(cell, rebuild);
        }
    } else {
        ++restore_epoch_;
        restored_cells_.clear();
        // Top level down, so a cell ends at its lowest popped save: its state when
        // level `level + 1` opened.
        for (std::size_t i = saves_.size(); i-- > mark;) {
            const SaveEntry& entry = saves_[i];
            const std::uint32_t cell = entry.cell;
            CellUndo& undo = undo_[cell];
            undo.save_stamp = entry.prev_stamp;
            if (undo.restore_stamp != restore_epoch_) {
                undo.restore_stamp = restore_epoch_;
                undo.restore_rebuild = 0;
                restored_cells_.push_back(cell);
            }
            if (entry.offset == rebuild_entry || entry.gen != undo.gen) {
                undo.restore_rebuild = 1;
            } else if (undo.restore_rebuild == 0) {
                copy_back(entry);
            }
        }
        for (const std::uint32_t cell : restored_cells_) {
            note(cell, undo_[cell].restore_rebuild != 0);
        }
    }
    saves_.resize(mark);
    cells_.drop_saves_from(level_cells_save_mark_[level + 1]);

    const std::size_t pivot_mark = level_pivot_mark_[level + 1];
    for (std::size_t i = pivot_mark; i < pivot_col_idxs_.size(); ++i) {
        pivot_row_[pivot_col_idxs_[i]] = nullptr;
    }
    pivot_col_idxs_.resize(pivot_mark);

    level_pivot_mark_.resize(level + 1);
    level_save_mark_.resize(level + 1);
    level_cells_save_mark_.resize(level + 1);
    level_stamp_.resize(level + 1);

    // Every `A` first, since a rebuilt `B` is reduced by its `A`.
    for (std::uint32_t kind = 0; kind < 2; ++kind) {
        for (const std::uint32_t restored : restored_by_kind_[kind]) {
            const std::uint32_t cell = (restored & ~std::uint32_t{1}) | kind;
            if (in_active_clause_[cell] == 0) {
                continue;
            }
            if ((restored & 1) != 0) {
                if (kind == 0) {
                    build_first(cell, watch_row(cell));
                } else {
                    build_second(cell, watch_row(cell));
                }
                // Built against this level's rows too, so a backjump below it
                // rebuilds again.
                push_rebuild_entry(cell, level);
            }
            place(cell);
        }
    }
    // A copied cell is as it stood at quiescence; only a rebuilt one can trigger.
    for (const std::uint32_t cell : rebuilt_cells_) {
        if (in_active_clause_[cell] != 0) {
            maybe_queue(cell >> 1);
        }
    }
}

template <GF2Vector V>
void ExactWatchlistTemplate<V>::update_with_backjump(std::size_t level, const Trail& trail) {
    assert(level == trail.level() && "the trail is not at the backjump's level");
    assert(level <= top_level() && "a backjump above the selector's top level");
    (void)trail;
    if (level == top_level()) {
        return;
    }
    // Propagation was quiescent before each dropped level's decision, so nothing
    // queued since holds any more, and every paused sweep is the top level's.
#ifndef NDEBUG
    const std::size_t top_pivot_mark = level_pivot_mark_[top_level()];
    for (std::size_t i = paused_head_; i < paused_end_; ++i) {
        assert(std::find(pivot_col_idxs_.begin() + static_cast<std::ptrdiff_t>(top_pivot_mark),
                         pivot_col_idxs_.end(),
                         paused_sweeps_[i].col_idx) != pivot_col_idxs_.end() &&
               "a sweep is paused below the top level");
    }
#endif
    drop_paused_sweeps();
    clear_side_queue();
    restore(level);
    maybe_clean_buckets();
    assert(pivot_col_idxs_.size() == trail.span().rows() &&
           "the selector's pivots disagree with the backjumped trail");
#ifndef NDEBUG
    if (debug_check_due(1)) {
        debug_check_cells(trail);
    }
#endif
}

template <GF2Vector V>
void ExactWatchlistTemplate<V>::update_with_substitution(std::span<const ClauseId> rewritten_clause_ids,
                                              const IndexSet& dropped_col_idxs,
                                              const Trail& trail) {
    assert(trail.span().rows() == 0 && trail.span().cols() == num_cols_ &&
           "a substitution leaves an empty trail over the same numbering");
    assert(top_level() == 0 && saves_.empty() && "a substitution runs at the root");
    assert(paused_head_ == paused_end_ && "a substitution runs at quiescence");
    assert(dropped_col_idxs.size() == num_cols_ &&
           "the dropped variables are over another numbering");
    num_dropped_vars_ = dropped_col_idxs.count();
    for (const std::uint32_t col_idx : pivot_col_idxs_) {
        pivot_row_[col_idx] = nullptr;
    }
    // No cell will lead on a dropped column again, so its bucket is never swept.
    dropped_col_idxs.for_each_set([this](const std::size_t col_idx) {
        bucket_entries_ -= buckets_[col_idx].size();
        buckets_[col_idx].clear();
    });
    pivot_col_idxs_.clear();
    clear_side_queue();

    for (const std::uint32_t pair : retired_pairs_) {
        // A narrowed clause has new cells by the time its old ones are freed.
        if (clause_pair_[pair_clause_[pair]] == pair) {
            clause_pair_[pair_clause_[pair]] = no_pair;
        }
        key_[2 * pair] = zero_key;
        key_[2 * pair + 1] = zero_key;
        free_pairs_.push_back(pair);
    }
    retired_pairs_.clear();
    // A clause the substitution left alone carries no pivot of the trail it
    // emptied, so its cells are already what building them now would give.
    for (const ClauseId id : rewritten_clause_ids) {
        if (database_.active_flags()[id] == 0) {
            continue;
        }
        const std::uint32_t pair = clause_pair_[id];
        assert(pair != no_pair && "an active clause has no cells");
        rebuild_pair(pair);
        place(2 * pair);
        if (pair_is_unit_[pair] == 0) {
            place(2 * pair + 1);
        }
        maybe_queue(pair);
    }
    maybe_clean_buckets();
#ifndef NDEBUG
    debug_check_cells(trail);
#endif
}

template <GF2Vector V>
void ExactWatchlistTemplate<V>::update_with_renumbering(const CompactionTransform& transform,
                                             const Trail& trail) {
    assert(trail.span().rows() == 0 && "a renumbering follows a substitution");
    assert(top_level() == 0 && saves_.empty() && "a renumbering runs at the root");
    assert(paused_head_ == paused_end_ && "a renumbering runs at quiescence");
    assert(pivot_col_idxs_.empty() && retired_pairs_.empty() &&
           "a renumbering follows the substitution's update");
    assert(transform.old_num_vars() + 1 == num_cols_ && "the transform is over another numbering");

    const std::uint32_t old_constant_key = constant_key();
    num_cols_ = trail.span().cols();
    num_dropped_vars_ = 0;
    const auto to_new = [&](const std::size_t col_idx) {
        return col_idx == old_constant_key ? num_cols_ - 1 : transform.to_new(col_idx);
    };
    cells_.relabel(transform, num_cols_);

    for (std::uint32_t& key : key_) {
        if (key != zero_key) {
            assert(to_new(key) != CompactionTransform::npos && "a cell leads on a dropped column");
            key = static_cast<std::uint32_t>(to_new(key));
        }
    }

    transform.move_by_column(
        buckets_, []([[maybe_unused]] const std::vector<std::uint32_t>& bucket) {
            assert(bucket.empty() && "a dropped column's bucket was not emptied");
        });
    assert(buckets_.size() == num_cols_ && "the buckets are not one per column");
    pivot_row_.assign(num_cols_, nullptr);
#ifndef NDEBUG
    debug_check_cells(trail);
#endif
}

#ifndef NDEBUG
template <GF2Vector V>
void ExactWatchlistTemplate<V>::debug_assert_cells_are_builds(std::uint32_t pair, bool with_first) {
    const std::uint32_t first = 2 * pair;
    for (const std::uint32_t cell : {first, first + 1}) {
        if (cell == first && !with_first) {
            continue;
        }
        const Equation taken = cells_.equation_of(cells_.cell(cell));
        const std::uint32_t taken_key = key_[cell];
        if (cell == first) {
            build_first(cell, watch_row(cell));
        } else {
            build_second(cell, watch_row(cell));
        }
        assert(taken_key == key_[cell] && "a reduction taken for a cell has another key");
        // A `B` taken beside a new `A` need only be congruent to the one built,
        // which `debug_check_cells()` checks.
        assert(((cell != first && with_first) ||
                cells_.equation_of(cells_.cell(cell)) == taken) &&
               "a reduction taken for a cell is not the cell built from its watch");
        cells_.copy(cells_.cell(cell), Cells::row_of(gf2_vector_of<V>(taken)));
    }
}

template <GF2Vector V>
bool ExactWatchlistTemplate<V>::debug_check_due(std::size_t every) {
    // A reduction per cell, a row per pivot.
    const std::size_t num_words = (num_cols_ + DenseGF2Vector::word_mask) >> DenseGF2Vector::word_shift;
    const std::size_t stride = every * (1 + pair_clause_.size() * num_words * num_words / 2048);
    return ++debug_events_ % stride == 0;
}

template <GF2Vector V>
void ExactWatchlistTemplate<V>::debug_check_cells(const Trail& trail) const {
    const RowSpace& span = trail.span();
    assert(span.cols() == num_cols_ && "the trail is over another numbering than the cells");
    const std::size_t num_vars = num_cols_ - 1;
    for (std::size_t col_idx = 0; col_idx < num_vars; ++col_idx) {
        assert(span.pivot_col_idxs().test(col_idx) == (pivot_row_[col_idx] != nullptr) &&
               "the selector's pivot table disagrees with the span");
    }
    std::vector<std::uint8_t> in_bucket(key_.size(), 0);
    std::size_t num_entries = 0;
    for (std::size_t col_idx = 0; col_idx < buckets_.size(); ++col_idx) {
        for (const std::uint32_t cell : buckets_[col_idx]) {
            in_bucket[cell] |= static_cast<std::uint8_t>(key_[cell] == col_idx);
        }
        num_entries += buckets_[col_idx].size();
    }
    assert(num_entries == bucket_entries_ && "the bucket entry count is off");
    assert(bucket_entries_ <= cleanup_threshold_ && "the buckets outgrew their cleanup");
    (void)num_entries;
    // Cells a paused sweep has yet to reach, still keyed on its column.
    std::vector<std::uint8_t> awaiting(key_.size(), 0);
    for (std::size_t i = paused_head_; i < paused_end_; ++i) {
        const PausedSweep& sweep = paused_sweeps_[i];
        assert(pivot_row_[sweep.col_idx] != nullptr && "a paused sweep's column is not a pivot");
        assert(sweep.cursor <= sweep.cells.size() && "a paused sweep ran past its bucket");
        for (std::size_t j = i + 1; j < paused_end_; ++j) {
            assert(paused_sweeps_[j].col_idx != sweep.col_idx && "a column is swept twice at once");
        }
        for (std::size_t j = sweep.cursor; j < sweep.cells.size(); ++j) {
            const std::uint32_t cell = sweep.cells[j];
            awaiting[cell] |= static_cast<std::uint8_t>(key_[cell] == sweep.col_idx);
        }
    }
    const auto cell_equation = [&](std::uint32_t cell) { return cells_.equation_of(cells_.cell(cell)); };
    const auto in_span = [&](Equation v) {
        span.reduce(v);
        return v.is_eq_true();
    };
    const auto keyed_on_pivot = [&](std::uint32_t cell) {
        const std::uint32_t key = key_[cell];
        return key != zero_key && key != constant_key() && span.pivot_col_idxs().test(key);
    };
    // The key a cell's partial reduction must end on, bucketed where it is a column.
    // A cell may instead still lead on a pivot while a paused sweep has yet to reach
    // it, or, if it `may_lag`, where a sweep left it.
    const auto check_key = [&](std::uint32_t cell, ReduceResult canonical, bool may_lag) {
        const std::uint32_t key = key_[cell];
        const bool lags = (may_lag || awaiting[cell] != 0) && keyed_on_pivot(cell);
        if (lags) {
            assert((in_bucket[cell] != 0 || awaiting[cell] != 0) &&
                   "a lagging cell is missing from its bucket");
        } else if (canonical.kind == ReduceResult::Kind::eq_true) {
            assert(key == zero_key && "a `0 = 0` cell carries a key");
        } else if (canonical.kind == ReduceResult::Kind::eq_false) {
            assert(key == constant_key() && "a `0 = 1` cell carries another key");
        } else {
            assert(key == canonical.bit && "a cell's key is not its leading non-pivot");
            assert(in_bucket[cell] != 0 && "a keyed cell is missing from its bucket");
        }
        const std::optional<std::size_t> lead = cell_equation(cell).leading_bit();
        assert((key == zero_key ? !lead.has_value() : lead == std::optional<std::size_t>(key)) &&
               "a cell does not lead on its key");
        (void)key;
        (void)lags;
        (void)lead;
    };
    for (ClauseId id = 0; id < database_.num_clauses(); ++id) {
        if (database_.active_flags()[id] == 0) {
            continue;
        }
        const std::uint32_t pair = clause_pair_[id];
        assert(pair != no_pair && "an active clause has no cells");
        const Clause& clause = database_.clauses()[id];
        const std::uint32_t first = 2 * pair;
        const bool unit = pair_is_unit_[pair] != 0;
        assert(unit == (clause.rows() == 1) && "a clause's width changed under its cells");

        const ClauseWatches& watches = database_.records()[id].watches;
        const Equation& base0 = clause.row(unit ? 0 : watches.watched0);
        const Equation first_eqn = cell_equation(first);
        Equation diff = first_eqn;
        diff.plus_assign(base0);
        assert(in_span(diff) && "`A` left its watch's coset of the span");
        Equation reduced = base0;
        check_key(first, span.partial_reduce(reduced),
                  !unit && key_[first + 1] == constant_key());
        assert((keyed_on_pivot(first) || first_eqn == reduced) &&
               "`A` is not its watch's partial reduction by the span");
        if (unit) {
            continue;
        }

        const std::uint32_t second = first + 1;
        const Equation& base1 = clause.row(watches.watched1);
        diff = cell_equation(second);
        diff.plus_assign(base1);
        Equation diff_with_first = diff;
        diff_with_first.plus_assign(first_eqn);
        assert((in_span(diff) || in_span(diff_with_first)) &&
               "`B` left its watch's coset of the span plus `A`");
        assert((key_[second] == zero_key || key_[second] != key_[first]) &&
               "`B` shares `A`'s key");
        reduced = base1;
        // Either cell may lag while the other is `0 = 1`; see `resume_sweeps`. A
        // lagging `A` may itself be `0 = 1` modulo the span, so beside it `B` is checked
        // only for leading on the constant.
        const std::uint32_t first_key = key_[first];
        if (keyed_on_pivot(first) && awaiting[first] == 0) {
            assert(key_[second] == constant_key() && "`A` lags while `B` is not `0 = 1`");
            assert(cell_equation(second).leading_bit() == std::optional<std::size_t>(num_vars) &&
                   "`B` does not lead on its key");
            continue;
        }
        // `B` was reduced by an `A` that has yet to be, and may lead where `A` will.
        if (keyed_on_pivot(first)) {
            const std::uint32_t second_key = key_[second];
            if (keyed_on_pivot(second)) {
                assert(awaiting[second] != 0 && "`B` leads on a pivot no sweep will reach it at");
            } else if (second_key != zero_key && second_key != constant_key()) {
                assert(in_bucket[second] != 0 && "a keyed cell is missing from its bucket");
            }
            const std::optional<std::size_t> lead = cell_equation(second).leading_bit();
            assert((second_key == zero_key ? !lead.has_value()
                                           : lead == std::optional<std::size_t>(second_key)) &&
                   "a cell does not lead on its key");
            (void)second_key;
            (void)lead;
            continue;
        }
        check_key(second, span.partial_reduce_with_extra(reduced, first_eqn),
                  first_key == constant_key());
    }
}
#endif

// The constructors alone, which bring in what they use: an explicit instantiation of
// the whole class keeps every inline helper as a function of its own, and GCC then
// declines to inline the cell operations into the sweep and the restore.
template ExactWatchlistTemplate<DenseGF2Vector>::ExactWatchlistTemplate(std::size_t,
                                                                        const ClauseDatabase&);
template ExactWatchlistTemplate<SparseGF2Vector>::ExactWatchlistTemplate(
    std::size_t, const ClauseDatabase&);

} // namespace xorcle
