#include "heuristics/selection/exact_watchlist.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

#include "algebra/equation_repr.h"
#include "algebra/reduce_result.h"

namespace xorcle {

namespace {

using Word = DenseGF2Vector::Word;

constexpr std::size_t npos = static_cast<std::size_t>(-1);

const Word* words_of(const Equation& equation) {
    return gf2_vector_of<DenseGF2Vector>(equation).words();
}

std::size_t leading_bit(const Word* words, std::size_t num_words) {
    for (std::size_t w = 0; w < num_words; ++w) {
        if (words[w] != 0) {
            return (w << 6) + static_cast<std::size_t>(std::countr_zero(words[w]));
        }
    }
    return npos;
}

/// @brief Add `row` into `cell`, both leading on `bit`; the first set bit after
/// `bit`, or `npos`.
std::size_t add_and_next(Word* cell, const Word* row, std::size_t num_words, std::size_t bit) {
    const std::size_t next = bit + 1;
    std::size_t found = npos;
    for (std::size_t w = bit >> 6; w < num_words; ++w) {
        const Word word = (cell[w] ^= row[w]);
        if (found != npos) {
            continue;
        }
        const Word above = (next >> 6) == w ? word & (~Word{0} << (next & 63)) : word;
        if (above != 0) {
            found = (w << 6) + static_cast<std::size_t>(std::countr_zero(above));
        }
    }
    return found;
}

} // namespace

ExactWatchlist::ExactWatchlist(std::size_t num_vars) { reset(num_vars + 1); }

#ifdef XORCLE_EXACT_WATCHLIST_TIMING
ExactWatchlist::~ExactWatchlist() {
    std::fprintf(stderr, "c exact_watchlist sweep_ns %lld\n",
                 static_cast<long long>(
                     std::chrono::duration_cast<std::chrono::nanoseconds>(sweep_time_).count()));
}
#endif

void ExactWatchlist::reset(std::size_t num_cols) {
    num_cols_ = num_cols;
    num_words_ = (num_cols + 63) / 64;
    words_.clear();
    key_.clear();
    gen_.clear();
    pair_clause_.clear();
    pair_is_unit_.clear();
    clause_pair_.assign(clauses_.size(), no_pair);
    buckets_.assign(num_cols, {});
    bucket_entries_ = 0;
    cleanup_threshold_ = num_cols;
    pivot_row_.assign(num_cols, nullptr);
    pivot_col_idxs_.clear();
    level_pivot_mark_.assign(1, 0);
    saves_.clear();
    save_words_.clear();
    level_save_mark_.assign(1, 0);
    level_save_word_mark_.assign(1, 0);
    level_stamp_.assign(1, 0);
    save_stamp_.clear();
    restore_stamp_.clear();
    restore_rebuild_.clear();
    restored_cells_.clear();
    clear_queue();
}

void ExactWatchlist::open_level() {
    level_pivot_mark_.push_back(pivot_col_idxs_.size());
    level_save_mark_.push_back(saves_.size());
    level_save_word_mark_.push_back(save_words_.size());
    level_stamp_.push_back(next_stamp_++);
}

std::uint32_t ExactWatchlist::reduce_by_span(Word* words, std::size_t lead,
                                             std::size_t& xors) const {
    while (lead != npos) {
        const Word* row = pivot_row_[lead];
        if (row == nullptr) {
            break;
        }
        lead = add_and_next(words, row, num_words_, lead);
        ++xors;
    }
    return lead == npos ? zero_key : static_cast<std::uint32_t>(lead);
}

std::uint32_t ExactWatchlist::reduce_by_span_and_first(Word* words, std::size_t lead,
                                                       const Word* first,
                                                       std::uint32_t first_key,
                                                       std::size_t& xors) const {
    while (lead != npos) {
        if (const Word* row = pivot_row_[lead]; row != nullptr) {
            lead = add_and_next(words, row, num_words_, lead);
        } else if (lead == first_key) {
            lead = add_and_next(words, first, num_words_, lead);
        } else {
            break;
        }
        ++xors;
    }
    return lead == npos ? zero_key : static_cast<std::uint32_t>(lead);
}

void ExactWatchlist::build_first(std::uint32_t cell) {
    const std::uint32_t pair = cell >> 1;
    const Clause& clause = clauses_[pair_clause_[pair]];
    Word* words = cell_words(cell);
    std::memcpy(words, words_of(clause.row(pair_is_unit_[pair] != 0 ? 0 : clause.watched0)),
                num_words_ * sizeof(Word));
    std::size_t xors = 0;
    key_[cell] = reduce_by_span(words, leading_bit(words, num_words_), xors);
}

void ExactWatchlist::build_second(std::uint32_t cell) {
    const Clause& clause = clauses_[pair_clause_[cell >> 1]];
    Word* words = cell_words(cell);
    std::memcpy(words, words_of(clause.row(clause.watched1)), num_words_ * sizeof(Word));
    std::size_t xors = 0;
    key_[cell] = reduce_by_span_and_first(words, leading_bit(words, num_words_),
                                          cell_words(cell - 1), key_[cell - 1], xors);
}

void ExactWatchlist::build_pair(ClauseId id) {
    const Clause& clause = clauses_[id];
    assert(clause.repr() == EquationRepr::dense && "the exact watchlist needs dense equations");
    assert(clause.cols() == num_cols_ && "a clause is over another numbering than the trail");
    assert(clause.rows() >= 1 && "a clause has no rows");
    assert(gf2_vector_of<DenseGF2Vector>(clause.row(0)).num_words() == num_words_);

    const auto pair = static_cast<std::uint32_t>(pair_clause_.size());
    const std::uint32_t first = 2 * pair;
    clause_pair_[id] = pair;
    pair_clause_.push_back(static_cast<std::uint32_t>(id));
    const bool unit = clause.rows() == 1;
    pair_is_unit_.push_back(unit ? 1 : 0);
    words_.resize(words_.size() + 2 * num_words_);
    for (int k = 0; k < 2; ++k) {
        key_.push_back(zero_key);
        gen_.push_back(0);
        save_stamp_.push_back(0);
        restore_stamp_.push_back(0);
        restore_rebuild_.push_back(0);
    }

    // Nothing to return to below the current level, so a backjump rebuilds.
    build_first(first);
    place(first);
    push_rebuild_entry(first, top_level());
    if (!unit) {
        build_second(first + 1);
        place(first + 1);
        push_rebuild_entry(first + 1, top_level());
    }
    maybe_queue(pair);
}

void ExactWatchlist::place(std::uint32_t cell) {
    const std::uint32_t key = key_[cell];
    if (key != zero_key && key != constant_key()) {
        buckets_[key].push_back(cell);
        ++bucket_entries_;
    }
}

void ExactWatchlist::maybe_clean_buckets() {
    counters_.peak_bucket_entries = std::max(counters_.peak_bucket_entries, bucket_entries_);
    if (bucket_entries_ <= cleanup_threshold_) {
        return;
    }
    ++counters_.cleanups;
    counters_.cleanup_entries += bucket_entries_;
    cleanup_kept_.resize(key_.size(), 0);
    std::size_t kept = 0;
    // Stable, so the entries left are swept in the order they were placed.
    for (std::size_t col = 0; col < buckets_.size(); ++col) {
        std::vector<std::uint32_t>& bucket = buckets_[col];
        std::size_t out = 0;
        for (const std::uint32_t cell : bucket) {
            if (key_[cell] == col && cleanup_kept_[cell] == 0 &&
                clause_active_[pair_clause_[cell >> 1]] != 0) {
                cleanup_kept_[cell] = 1;
                bucket[out++] = cell;
            }
        }
        bucket.resize(out);
        kept += out;
    }
    for (const std::vector<std::uint32_t>& bucket : buckets_) {
        for (const std::uint32_t cell : bucket) {
            cleanup_kept_[cell] = 0;
        }
    }
    counters_.cleanup_dropped += bucket_entries_ - kept;
    bucket_entries_ = kept;
    cleanup_threshold_ = 2 * kept + num_cols_;
}

void ExactWatchlist::save(std::uint32_t cell, std::size_t level) {
    if (level == 0 || save_stamp_[cell] == level_stamp_[level]) {
        return;
    }
    saves_.push_back(
        SaveEntry{cell, gen_[cell], key_[cell], save_stamp_[cell], save_words_.size()});
    const Word* words = cell_words(cell);
    save_words_.insert(save_words_.end(), words, words + num_words_);
    save_stamp_[cell] = level_stamp_[level];
    ++counters_.saves;
}

void ExactWatchlist::push_rebuild_entry(std::uint32_t cell, std::size_t level) {
    // An entry already at this level rebuilds once `gen_` has moved past it.
    if (level == 0 || save_stamp_[cell] == level_stamp_[level]) {
        return;
    }
    saves_.push_back(SaveEntry{cell, gen_[cell], key_[cell], save_stamp_[cell], rebuild_entry});
    save_stamp_[cell] = level_stamp_[level];
}

void ExactWatchlist::sweep(std::size_t col_idx, const Word* row, std::size_t level) {
    sweep_scratch_.swap(buckets_[col_idx]);
    bucket_entries_ -= sweep_scratch_.size();
    for (const std::uint32_t cell : sweep_scratch_) {
        if (key_[cell] != col_idx) {
            continue;
        }
        const std::uint32_t pair = cell >> 1;
        if (clause_active_[pair_clause_[pair]] == 0) {
            continue;
        }
        // A pair with a `0 = 1` cell cannot trigger: an `A` of `0 = 1` satisfies the
        // clause, and a `B` of `0 = 1` means the second watch or the sum of both is
        // `0 = 1`, so either watch entering the span satisfies it by the other. So the
        // partner stays in this bucket
        // unreduced. Watches move only on a clause that can trigger, so only a backjump
        // changes the `0 = 1` cell, and one below this row leaves the partner a partial
        // reduction in its bucket again.
        if (key_[cell ^ 1] == constant_key()) {
            buckets_[col_idx].push_back(cell);
            ++bucket_entries_;
            ++counters_.skips;
            continue;
        }
        ++counters_.touches;
        save(cell, level);
        Word* words = cell_words(cell);
        const std::size_t lead = add_and_next(words, row, num_words_, col_idx);
        ++counters_.xors;

        if ((cell & 1) != 0) {
            const std::uint32_t key = reduce_by_span_and_first(
                words, lead, cell_words(cell - 1), key_[cell - 1], counters_.xors);
            key_[cell] = key;
            place(cell);
            if (key == zero_key) {
                maybe_queue(pair);
            }
            continue;
        }

        const std::uint32_t key = reduce_by_span(words, lead, counters_.xors);
        key_[cell] = key;
        place(cell);
        if (pair_is_unit_[pair] != 0) {
            if (key != constant_key()) {
                maybe_queue(pair);
            }
            continue;
        }
        // `B` may now lead where `A` does, which `B`'s reduction by `A` forbids.
        const std::uint32_t second = cell + 1;
        bool second_is_zero = false;
        if (key != zero_key && key_[second] == key) {
            ++counters_.fixups;
            save(second, level);
            Word* second_words = cell_words(second);
            const std::size_t second_lead = add_and_next(second_words, words, num_words_, key);
            ++counters_.xors;
            key_[second] =
                reduce_by_span_and_first(second_words, second_lead, words, key, counters_.xors);
            place(second);
            second_is_zero = key_[second] == zero_key;
        }
        if (key == zero_key || second_is_zero) {
            maybe_queue(pair);
        }
    }
    sweep_scratch_.clear();
}

ExactWatchlist::Trigger ExactWatchlist::trigger(std::uint32_t pair) const {
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

void ExactWatchlist::maybe_queue(std::uint32_t pair) {
    const ClauseId id = pair_clause_[pair];
    if (in_queue_[id] != 0 || trigger(pair) == Trigger::none) {
        return;
    }
    in_queue_[id] = 1;
    queue_.push_back(id);
    ++counters_.triggers;
}

void ExactWatchlist::clear_queue() {
    for (std::size_t i = queue_head_; i < queue_.size(); ++i) {
        in_queue_[queue_[i]] = 0;
    }
    queue_.clear();
    queue_head_ = 0;
}

std::optional<ClauseId> ExactWatchlist::next() {
    const std::optional<ClauseSelection> selection = next_with_hint();
    if (!selection.has_value()) {
        return std::nullopt;
    }
    return selection->id;
}

std::optional<ClauseSelection> ExactWatchlist::next_with_hint() {
    while (queue_head_ < queue_.size()) {
        const ClauseId id = queue_[queue_head_++];
        in_queue_[id] = 0;
        if (clause_active_[id] == 0) {
            continue;
        }
        assert(clause_pair_[id] != no_pair && "an active clause has no cells");
        switch (trigger(clause_pair_[id])) {
            case Trigger::none:
                continue;
            case Trigger::unit:
                return ClauseSelection{id, std::nullopt};
            case Trigger::zero:
                return ClauseSelection{id, WatchType::zero};
            case Trigger::one:
                return ClauseSelection{id, WatchType::one};
        }
    }
    queue_.clear();
    queue_head_ = 0;
    return std::nullopt;
}

void ExactWatchlist::add_clause(Clause clause) {
    const ClauseId id = clauses_.size();
    ClauseSelector::add_clause(std::move(clause));
    in_queue_.push_back(0);
    clause_pair_.push_back(no_pair);
    build_pair(id);
}

void ExactWatchlist::update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) {
    const RowSpace& span = trail.span();
    assert(span.repr() == EquationRepr::dense && "the exact watchlist needs dense equations");
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
        open_level();
    }
    const Equation& row = span.row(trail.trail_idx_to_row_idx()[trail_idx]);
    assert(row == reduced_eqn && "the reported equation is not the row the trail stores");
    (void)reduced_eqn;

    const Word* words = words_of(row);
    const std::size_t lead = leading_bit(words, num_words_);
    assert(lead != npos && "a stored trail row is `0 = 0`");
    if (lead == constant_key()) {
        // The conflict: a backjump follows, and nothing leads on the constant.
        return;
    }
    assert(pivot_row_[lead] == nullptr && "a new trail row leads on a pivot");
    pivot_row_[lead] = words;
    pivot_col_idxs_.push_back(static_cast<std::uint32_t>(lead));
#ifdef XORCLE_EXACT_WATCHLIST_TIMING
    const auto start = std::chrono::steady_clock::now();
    sweep(lead, words, level);
    sweep_time_ += std::chrono::steady_clock::now() - start;
#else
    sweep(lead, words, level);
#endif
    maybe_clean_buckets();
#ifndef NDEBUG
    if (debug_check_due(16)) {
        debug_check_cells(trail);
    }
#endif
}

void ExactWatchlist::update_with_new_watch(ClauseId id, ClauseRowIdx, WatchType type,
                                           const Trail& trail) {
    assert(is_clause_active(id) && "a watch moved on an inactive clause");
    assert(clauses_[id].rows() >= 2 && "a watch moved on a unit clause");
    assert(type != WatchType::sum && "sum triggers are normalized to one before reaching a selector");
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
    // `B` is reduced by `A`, so a new first watch rebuilds both.
    if (type == WatchType::zero) {
        ++gen_[first];
        build_first(first);
        place(first);
        push_rebuild_entry(first, level);
        ++counters_.rebuilds;
    }
    ++gen_[first + 1];
    build_second(first + 1);
    place(first + 1);
    push_rebuild_entry(first + 1, level);
    ++counters_.rebuilds;
    maybe_queue(pair);
    maybe_clean_buckets();
}

void ExactWatchlist::restore(std::size_t level) {
    ++restore_epoch_;
    restored_cells_.clear();
    const std::size_t mark = level_save_mark_[level + 1];
    // Top level down, so a cell ends at its lowest popped save: its state when
    // level `level + 1` opened.
    for (std::size_t i = saves_.size(); i-- > mark;) {
        const SaveEntry& entry = saves_[i];
        const std::uint32_t cell = entry.cell;
        save_stamp_[cell] = entry.prev_stamp;
        if (restore_stamp_[cell] != restore_epoch_) {
            restore_stamp_[cell] = restore_epoch_;
            restore_rebuild_[cell] = 0;
            restored_cells_.push_back(cell);
        }
        // A saved state from before a watch move is of another watch.
        if (entry.word_offset == rebuild_entry || entry.gen != gen_[cell]) {
            restore_rebuild_[cell] = 1;
        } else if (restore_rebuild_[cell] == 0) {
            std::memcpy(cell_words(cell), save_words_.data() + entry.word_offset,
                        num_words_ * sizeof(Word));
            key_[cell] = entry.key;
            ++counters_.restores;
        }
    }
    saves_.resize(mark);
    save_words_.resize(level_save_word_mark_[level + 1]);

    const std::size_t pivot_mark = level_pivot_mark_[level + 1];
    for (std::size_t i = pivot_mark; i < pivot_col_idxs_.size(); ++i) {
        pivot_row_[pivot_col_idxs_[i]] = nullptr;
    }
    pivot_col_idxs_.resize(pivot_mark);

    level_pivot_mark_.resize(level + 1);
    level_save_mark_.resize(level + 1);
    level_save_word_mark_.resize(level + 1);
    level_stamp_.resize(level + 1);

    // Every `A` first, since a rebuilt `B` is reduced by its `A`.
    for (std::uint32_t kind = 0; kind < 2; ++kind) {
        for (const std::uint32_t cell : restored_cells_) {
            if ((cell & 1) != kind || clause_active_[pair_clause_[cell >> 1]] == 0) {
                continue;
            }
            if (restore_rebuild_[cell] != 0) {
                if (kind == 0) {
                    build_first(cell);
                } else {
                    build_second(cell);
                }
                // Built against this level's rows too, so a backjump below it
                // rebuilds again.
                push_rebuild_entry(cell, level);
                ++counters_.rebuilds;
            }
            place(cell);
        }
    }
    // A copied cell is as it stood at quiescence; only a rebuilt one can trigger.
    for (const std::uint32_t cell : restored_cells_) {
        if (restore_rebuild_[cell] != 0 && clause_active_[pair_clause_[cell >> 1]] != 0) {
            maybe_queue(cell >> 1);
        }
    }
}

void ExactWatchlist::update_with_backjump(std::size_t level, const Trail& trail) {
    assert(level == trail.level() && "the trail is not at the backjump's level");
    assert(level <= top_level() && "a backjump above the selector's top level");
    (void)trail;
    if (level == top_level()) {
        return;
    }
    // Propagation was quiescent before each dropped level's decision, so nothing
    // queued since holds any more.
    clear_queue();
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

void ExactWatchlist::update_with_compaction(const CompactionTransform&, const Trail& trail) {
    assert(trail.span().rows() == 0 && "compaction leaves an empty trail");
    assert(top_level() == 0 && saves_.empty() && "compaction runs at the root");
    reset(trail.span().cols());
    for (ClauseId id = 0; id < clauses_.size(); ++id) {
        if (clause_active_[id] != 0) {
            build_pair(id);
        }
    }
    cleanup_threshold_ = 2 * bucket_entries_ + num_cols_;
#ifndef NDEBUG
    debug_check_cells(trail);
#endif
}

void ExactWatchlist::add_to_stats(RunStats& stats) const {
    stats.exact_touches = counters_.touches;
    stats.exact_skips = counters_.skips;
    stats.exact_fixups = counters_.fixups;
    stats.exact_xors = counters_.xors;
    stats.exact_triggers = counters_.triggers;
    stats.exact_saves = counters_.saves;
    stats.exact_restores = counters_.restores;
    stats.exact_rebuilds = counters_.rebuilds;
    stats.exact_cleanups = counters_.cleanups;
    stats.exact_cleanup_entries = counters_.cleanup_entries;
    stats.exact_cleanup_dropped = counters_.cleanup_dropped;
    stats.exact_peak_bucket_entries = counters_.peak_bucket_entries;
}

#ifndef NDEBUG
bool ExactWatchlist::debug_check_due(std::size_t every) {
    // A reduction per cell, a row per pivot.
    const std::size_t stride = every * (1 + pair_clause_.size() * num_words_ * num_words_ / 2048);
    return ++debug_events_ % stride == 0;
}

void ExactWatchlist::debug_check_cells(const Trail& trail) const {
    const RowSpace& span = trail.span();
    assert(span.cols() == num_cols_ && "the trail is over another numbering than the cells");
    const std::size_t num_vars = num_cols_ - 1;
    for (std::size_t col = 0; col < num_vars; ++col) {
        assert(span.pivot_col_idxs().test(col) == (pivot_row_[col] != nullptr) &&
               "the selector's pivot table disagrees with the span");
    }
    std::vector<std::uint8_t> in_bucket(key_.size(), 0);
    std::size_t num_entries = 0;
    for (std::size_t col = 0; col < buckets_.size(); ++col) {
        for (const std::uint32_t cell : buckets_[col]) {
            in_bucket[cell] |= key_[cell] == col ? 1 : 0;
        }
        num_entries += buckets_[col].size();
    }
    assert(num_entries == bucket_entries_ && "the bucket entry count is off");
    assert(bucket_entries_ <= cleanup_threshold_ && "the buckets outgrew their cleanup");
    (void)num_entries;
    const auto cell_equation = [&](std::uint32_t cell) {
        std::vector<std::size_t> bits;
        const Word* words = cell_words(cell);
        for (std::size_t w = 0; w < num_words_; ++w) {
            for (Word word = words[w]; word != 0; word &= word - 1) {
                bits.push_back((w << 6) + static_cast<std::size_t>(std::countr_zero(word)));
            }
        }
        return Equation::from_bits(EquationRepr::dense, num_vars, bits);
    };
    const auto in_span = [&](Equation v) {
        span.reduce(v);
        return v.is_eq_true();
    };
    // The key a cell's partial reduction must end on, bucketed where it is a column.
    // A cell that `may_lag` may instead still lead on the pivot a sweep left it at.
    const auto check_key = [&](std::uint32_t cell, ReduceResult canonical, bool may_lag) {
        const std::uint32_t key = key_[cell];
        const bool lags = may_lag && key != zero_key && key != constant_key() &&
                          span.pivot_col_idxs().test(key);
        if (lags) {
            assert(in_bucket[cell] != 0 && "a lagging cell is missing from its bucket");
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
    for (ClauseId id = 0; id < clauses_.size(); ++id) {
        if (clause_active_[id] == 0) {
            continue;
        }
        const std::uint32_t pair = clause_pair_[id];
        assert(pair != no_pair && "an active clause has no cells");
        const Clause& clause = clauses_[id];
        const std::uint32_t first = 2 * pair;
        const bool unit = pair_is_unit_[pair] != 0;
        assert(unit == (clause.rows() == 1) && "a clause's width changed under its cells");

        const Equation& base0 = clause.row(unit ? 0 : clause.watched0);
        const Equation first_eqn = cell_equation(first);
        Equation diff = first_eqn;
        diff.plus_assign(base0);
        assert(in_span(diff) && "`A` left its watch's coset of the span");
        Equation reduced = base0;
        check_key(first, span.partial_reduce(reduced),
                  !unit && key_[first + 1] == constant_key());
        if (unit) {
            continue;
        }

        const std::uint32_t second = first + 1;
        const Equation& base1 = clause.row(clause.watched1);
        diff = cell_equation(second);
        diff.plus_assign(base1);
        Equation diff_with_first = diff;
        diff_with_first.plus_assign(first_eqn);
        assert((in_span(diff) || in_span(diff_with_first)) &&
               "`B` left its watch's coset of the span plus `A`");
        assert((key_[second] == zero_key || key_[second] != key_[first]) &&
               "`B` shares `A`'s key");
        reduced = base1;
        // Either cell may lag while the other is `0 = 1`; see `sweep`. A lagging `A`
        // may itself be `0 = 1` modulo the span, so beside it `B` is checked only for
        // leading on the constant.
        const std::uint32_t first_key = key_[first];
        if (first_key != zero_key && first_key != constant_key() &&
            span.pivot_col_idxs().test(first_key)) {
            assert(key_[second] == constant_key() && "`A` lags while `B` is not `0 = 1`");
            assert(cell_equation(second).leading_bit() == std::optional<std::size_t>(num_vars) &&
                   "`B` does not lead on its key");
            continue;
        }
        check_key(second, span.partial_reduce_with_extra(reduced, first_eqn),
                  first_key == constant_key());
    }
}
#endif

} // namespace xorcle
