#pragma once

/// @file exact_watchlist.h
/// @brief The selector that queues exactly the clauses whose watch pair became dependent.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/dense/dense_gf2_vector.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "core/watch.h"
#include "heuristics/selection/clause_selector.h"

namespace xorcle {

struct ExactWatchlistCounters {
    std::size_t touches = 0; ///< Cells a sweep added its trail row to.
    std::size_t skips = 0; ///< Cells a sweep left alone because their pair cannot trigger.
    std::size_t fixups = 0; ///< Second cells reduced by their first after its key moved onto theirs.
    std::size_t xors = 0; ///< Row additions made by sweeps and fix-ups.
    std::size_t triggers = 0; ///< Clauses queued.
    std::size_t saves = 0; ///< Cells copied onto a level's save stack.
    std::size_t restores = 0; ///< Saved cells a backjump copied back.
    std::size_t rebuilds = 0; ///< Cells rebuilt from their watches, on a watch move or a backjump.
    std::size_t cleanups = 0; ///< Sweeps of every bucket for stale entries.
    std::size_t cleanup_entries = 0; ///< Bucket entries those sweeps read.
    std::size_t cleanup_dropped = 0; ///< Stale bucket entries those sweeps dropped.
    std::size_t peak_bucket_entries = 0;
};

/// @brief Queues a clause exactly when its watch pair becomes dependent modulo the
/// trail span, with that trigger as the hint.
///
/// Each active clause keeps two cells: `A`, its first watch partially reduced by the
/// span, and `B`, its second watch partially reduced by the span and `A`. The pair is
/// independent exactly when neither is `0 = 0`. Each cell is bucketed on its leading
/// column; a new trail row sweeps the bucket of its own leading column, and a backjump
/// copies back the cells the dropped levels changed, as each stood before its first
/// change there. While either cell is `0 = 1` the pair cannot trigger, and the other
/// may lag the span.
///
/// @pre Dense equations, and complete propagation: every learned equation is the last
/// row of the trail, at its top level.
class ExactWatchlist final : public ClauseSelector {
public:
    /// @param num_vars The variable count of the starting clauses.
    explicit ExactWatchlist(std::size_t num_vars);

#ifdef XORCLE_EXACT_WATCHLIST_TIMING
    /// @brief Print the time spent sweeping to stderr.
    ~ExactWatchlist() override;
#endif

    std::optional<ClauseId> next() override;

    /// @brief The next queued clause still triggered and not known satisfied, or
    /// `std::nullopt` at quiescence.
    std::optional<ClauseSelection> next_with_hint() override;

    /// @pre `reduced_eqn` is the row `trail` stores for its last equation, and every
    /// earlier one was reported here too.
    void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) override;

    /// @pre `id` names an active clause of at least two rows.
    void update_with_new_watch(ClauseId id, ClauseRowIdx old_watch, WatchType type,
                               const Trail& trail) override;

    void update_with_backjump(std::size_t level, const Trail& trail) override;

    /// @pre `trail` is empty.
    void update_with_compaction(const CompactionTransform& transform, const Trail& trail) override;

    const ExactWatchlistCounters& counters() const noexcept { return counters_; }

    void add_to_stats(RunStats& stats) const override;

protected:
    /// @brief Store a clause, build its cells against the trail as last reported, and
    /// queue it if it is already triggered.
    void add_clause(Clause clause) override;

private:
    using Word = DenseGF2Vector::Word;

    enum class Trigger : std::uint8_t {
        none,
        unit, ///< A unit clause not yet satisfied.
        zero, ///< `A` is `0 = 0`.
        one ///< `B` is `0 = 0`: the second watch is in the span plus the first.
    };

    /// @brief A cell's words and key as they stood before its first change at a level,
    /// or, with `word_offset == rebuild_entry`, a mark that it has no such state to
    /// return to and must be rebuilt.
    struct SaveEntry {
        std::uint32_t cell;
        std::uint32_t gen;
        std::uint32_t key;
        std::uint32_t prev_stamp;
        std::size_t word_offset;
    };

    static constexpr std::uint32_t zero_key = UINT32_MAX; ///< The cell is `0 = 0`.
    static constexpr std::uint32_t no_pair = UINT32_MAX;
    static constexpr std::size_t rebuild_entry = static_cast<std::size_t>(-1);

    Word* cell_words(std::uint32_t cell) { return words_.data() + cell * num_words_; }
    const Word* cell_words(std::uint32_t cell) const { return words_.data() + cell * num_words_; }

    std::uint32_t constant_key() const { return static_cast<std::uint32_t>(num_cols_ - 1); }
    std::size_t top_level() const { return level_stamp_.size() - 1; }

    /// @brief Forget every cell and pivot, over `num_cols` columns.
    void reset(std::size_t num_cols);

    void open_level();

    void build_pair(ClauseId id);
    void build_first(std::uint32_t cell);
    void build_second(std::uint32_t cell);

    /// @brief Reduce `words`, leading on `lead`, by the span until its lead is not a
    /// pivot; the key it ends on.
    std::uint32_t reduce_by_span(Word* words, std::size_t lead, std::size_t& xors) const;

    /// @brief As `reduce_by_span`, and by `first` too while the lead is `first_key`.
    std::uint32_t reduce_by_span_and_first(Word* words, std::size_t lead, const Word* first,
                                           std::uint32_t first_key, std::size_t& xors) const;

    void place(std::uint32_t cell);

    /// @brief Drop every stale bucket entry once there are more than twice as
    /// many entries as the last cleanup kept, plus one per column.
    void maybe_clean_buckets();

    void save(std::uint32_t cell, std::size_t level);
    void push_rebuild_entry(std::uint32_t cell, std::size_t level);

    void sweep(std::size_t col_idx, const Word* row, std::size_t level);

    Trigger trigger(std::uint32_t pair) const;
    void maybe_queue(std::uint32_t pair);
    void clear_queue();

    /// @brief Copy back every cell the levels above `level` saved, and rebuild the
    /// ones they cannot restore.
    void restore(std::size_t level);

#ifndef NDEBUG
    /// @brief Whether this event's full check is due: `every` times as rarely
    /// as a check costs, relative to a small database.
    bool debug_check_due(std::size_t every);
    void debug_check_cells(const Trail& trail) const;
#endif

    std::size_t num_cols_ = 0;
    std::size_t num_words_ = 0;

    /// @brief Two cells per pair, `A` then `B`; a unit clause uses only `A`.
    std::vector<Word> words_;
    std::vector<std::uint32_t> key_;
    std::vector<std::uint32_t> gen_; ///< Bumped whenever a cell's watch changes.
    std::vector<std::uint32_t> pair_clause_;
    std::vector<std::uint8_t> pair_is_unit_;
    std::vector<std::uint32_t> clause_pair_; ///< `no_pair` for a clause that had none built.

    std::vector<std::vector<std::uint32_t>> buckets_; ///< Per column, cells that may lead on it.
    std::size_t bucket_entries_ = 0;
    std::size_t cleanup_threshold_ = 0;
    std::vector<std::uint8_t> cleanup_kept_; ///< Per cell, whether this cleanup kept an entry.
    std::vector<std::uint32_t> sweep_scratch_;

    std::vector<const Word*> pivot_row_; ///< Per column, the trail row pivoting on it.
    std::vector<std::uint32_t> pivot_col_idxs_; ///< In trail order, with the constant's omitted.
    std::vector<std::size_t> level_pivot_mark_;

    std::vector<SaveEntry> saves_;
    std::vector<Word> save_words_;
    std::vector<std::size_t> level_save_mark_;
    std::vector<std::size_t> level_save_word_mark_;
    std::vector<std::uint32_t> level_stamp_; ///< Fresh at every opening of a level.
    std::uint32_t next_stamp_ = 1;
    std::vector<std::uint32_t> save_stamp_; ///< Per cell, the stamp of the level it last saved at.
    std::vector<std::uint32_t> restore_stamp_;
    std::vector<std::uint8_t> restore_rebuild_;
    std::uint32_t restore_epoch_ = 0;
    std::vector<std::uint32_t> restored_cells_;

    std::vector<ClauseId> queue_;
    std::size_t queue_head_ = 0;
    std::vector<std::uint8_t> in_queue_;

    ExactWatchlistCounters counters_;

#ifdef XORCLE_EXACT_WATCHLIST_TIMING
    std::chrono::steady_clock::duration sweep_time_{};
#endif

#ifndef NDEBUG
    std::size_t debug_events_ = 0;
#endif
};

} // namespace xorcle
