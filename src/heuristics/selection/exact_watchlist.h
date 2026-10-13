#pragma once

/// @file exact_watchlist.h
/// @brief The selector that queues exactly the clauses whose watch pair became dependent.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
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
    std::size_t touches_ahead = 0; ///< Touches of a triggered pair's partner ahead of its sweep.
    std::size_t skips = 0; ///< Cells a sweep left alone because their pair cannot trigger.
    std::size_t fixups = 0; ///< Second cells reduced by their first after its key moved onto theirs.
    std::size_t row_additions = 0; ///< Row additions made by sweeps and fix-ups.
    std::size_t triggers = 0; ///< Clauses a sweep found triggered, or queued on the side.
    std::size_t saves = 0; ///< Cells copied onto a level's save stack.
    std::size_t restores = 0; ///< Saved cells a backjump copied back.
    std::size_t rebuilds = 0; ///< Cells rebuilt from their watches, on a watch move or a backjump.
    std::size_t cleanups = 0; ///< Sweeps of every bucket for stale entries.
    std::size_t cleanup_entries = 0; ///< Bucket entries those sweeps read.
    std::size_t cleanup_dropped = 0; ///< Stale bucket entries those sweeps dropped.
    std::size_t peak_bucket_entries = 0;
    std::size_t sweeps_started = 0;
    std::size_t sweeps_finished = 0;
    std::size_t sweeps_dropped = 0; ///< Paused sweeps a backjump dropped.
};

/// @brief Queues a clause exactly when its watch pair becomes dependent modulo the
/// trail span, with that trigger as the hint.
///
/// Each active clause keeps two cells: `A`, its first watch partially reduced by the
/// span, and `B`, its second watch partially reduced by the span and `A`. The pair is
/// independent exactly when neither is `0 = 0`. Each cell is bucketed on its leading
/// column. A new trail row becomes a pivot at once and starts a paused sweep of the
/// bucket of its column; the oldest paused sweep resumes when a trigger is wanted and
/// pauses again at the first. A backjump drops the paused sweeps and copies back the
/// cells the dropped levels changed, as each stood before its first change there. A
/// cell may lead on a pivot while a paused sweep of that column has yet to reach it, or
/// while its partner is `0 = 1` and the pair cannot trigger.
///
/// The propagator reads the cell a trigger leaves standing and writes the cells of a
/// watch it moves. A `B` it wrote after reading one may differ from the reduction of
/// its watch by an element of the span, and leads where that does.
///
/// @pre Dense equations, and complete propagation: every learned equation is the last
/// row of the trail, at its top level.
class ExactWatchlist final : public ClauseSelector {
public:
    /// @param num_vars The variable count of the starting clauses.
    explicit ExactWatchlist(std::size_t num_vars);

    /// @brief The next triggered clause: first from the side queue, else from the
    /// oldest paused sweep; `std::nullopt` once both are empty.
    std::optional<ClauseSelection> next() override;

    /// @brief Copy out `A` for a `WatchType::one` trigger, which is exact, and `B` for a
    /// `WatchType::zero` one, which is congruent.
    /// @pre `next()` last returned `id` with this trigger, and nothing was reported since.
    HeldReduction copy_reduced_companion(ClauseId id, WatchType type, Equation& reduced,
                                         ReduceResult& result) override;

    /// @brief Take the cells of the moved watch from `reductions`.
    /// @pre `id` names an active clause of at least two rows.
    void update_with_new_watch(ClauseId id, ClauseRowIdx old_watch_row_idx, WatchType type,
                               const Trail& trail, const WatchReductions& reductions) override;

    void update_with_backjump(std::size_t level, const Trail& trail) override;

    /// @pre `trail` is empty, and the selector is at the root with nothing left to select.
    void update_with_substitution(std::span<const ClauseId> rewritten_clause_ids,
                                  const IndexSet& dropped_col_idxs, const Trail& trail) override;

    /// @brief Relabel every cell, key and bucket; no watch moves and no bucket is reordered.
    /// @pre `trail` is empty, and `update_with_substitution()` ran since the last
    /// variable was dropped.
    void update_with_renumbering(const CompactionTransform& transform, const Trail& trail) override;

    const ExactWatchlistCounters& counters() const noexcept { return counters_; }

    void add_to_stats(RunStats& stats) const override;

protected:
    /// @brief Store a clause, build its cells against the trail as last reported, and
    /// queue it on the side if it is already triggered.
    void add_clause(Clause clause) override;

    /// @pre `reduced_eqn` is the row `trail` stores for its last equation, and every
    /// earlier one was reported here too; a new level opens only at quiescence.
    void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) override;

    void update_with_inactive_clause(ClauseId id) override;

private:
    using Word = DenseGF2Vector::Word;

    enum class Trigger : std::uint8_t {
        none,
        unit, ///< A unit clause not yet satisfied.
        zero, ///< `A` is `0 = 0`.
        one ///< `B` is `0 = 0`: the second watch is in the span plus the first.
    };

    /// @brief The swept-out bucket of a pivot column, touched up to `cursor`.
    struct PausedSweep {
        std::uint32_t col_idx = 0;
        std::size_t cursor = 0;
        std::vector<std::uint32_t> cells;
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
    const Word* watch_words(std::uint32_t cell) const {
        return watch_words_.data() + cell * num_words_;
    }

    std::uint32_t constant_key() const { return static_cast<std::uint32_t>(num_cols_ - 1); }
    std::size_t top_level() const { return level_stamp_.size() - 1; }

    /// @brief Forget every cell and pivot, over `num_cols` columns.
    void reset(std::size_t num_cols);

    void open_level();

    void build_pair(ClauseId id);

    /// @brief Build a pair's cells again, from its clause as it now stands.
    /// @pre The selector is at the root, with no pivot.
    void rebuild_pair(std::uint32_t pair);
    const Word* watch_row(std::uint32_t cell) const;
    void load_watch(std::uint32_t cell);
    void build_first(std::uint32_t cell, const Word* watch);
    void build_second(std::uint32_t cell, const Word* watch);

    /// @brief Make `reduced` the cell, in place of building it from its watch.
    void take_reduction(std::uint32_t cell, const Equation& reduced, const ReduceResult& result);

    /// @brief Reduce `words`, leading on `lead`, by the span until its lead is not a
    /// pivot; the key it ends on.
    std::uint32_t reduce_by_span(Word* words, std::size_t lead, std::size_t& row_additions) const;

    /// @brief As `reduce_by_span`, and by `first` too while the lead is `first_key`.
    std::uint32_t reduce_by_span_and_first(Word* words, std::size_t lead, const Word* first,
                                           std::uint32_t first_key,
                                           std::size_t& row_additions) const;

    void place(std::uint32_t cell) {
        const std::uint32_t key = key_[cell];
        if (key != zero_key && key != constant_key()) {
            buckets_[key].push_back(cell);
            ++bucket_entries_;
        }
    }

    /// @brief Drop every stale bucket entry once there are more than twice as
    /// many entries as the last cleanup kept, plus one per column no substitution
    /// has emptied.
    void maybe_clean_buckets() {
        counters_.peak_bucket_entries = std::max(counters_.peak_bucket_entries, bucket_entries_);
        if (bucket_entries_ > cleanup_threshold_) {
            clean_buckets();
        }
    }

    void clean_buckets();

    void save(std::uint32_t cell, std::size_t level);
    void push_rebuild_entry(std::uint32_t cell, std::size_t level);

    /// @brief Reduce a cell leading on a pivot until it does not, fixing up `B` after
    /// `A`; whether its pair may have become triggered.
    bool advance(std::uint32_t cell, std::size_t level);

    /// @brief Resume the oldest paused sweeps until one touches a pair into a trigger.
    std::optional<ClauseSelection> resume_sweeps();

    /// @brief Put back what each paused sweep has yet to reach, and forget them.
    void drop_paused_sweeps();

    /// @brief Advance a triggered pair's cell that a paused sweep has yet to reach.
    void bring_current(std::uint32_t pair);

    Trigger trigger(std::uint32_t pair) const;
    static std::optional<ClauseSelection> selection_of(ClauseId id, Trigger trigger);
    void maybe_queue(std::uint32_t pair);
    void clear_side_queue();

    /// @brief Copy back every cell the levels above `level` saved, and rebuild the
    /// ones they cannot restore.
    void restore(std::size_t level);

#ifndef NDEBUG
    /// @brief Whether this event's full check is due: `every` times as rarely
    /// as a check costs, relative to a small database.
    bool debug_check_due(std::size_t every);
    void debug_check_cells(const Trail& trail) const;

    /// @brief Check a pair's cells against building them from their watch copies,
    /// `A` only if `with_first`.
    void debug_assert_cells_are_builds(std::uint32_t pair, bool with_first);
#endif

    std::size_t num_cols_ = 0;
    std::size_t num_words_ = 0;
    std::size_t num_dropped_vars_ = 0; ///< Since the last renumbering.

    /// @brief Two cells per pair, `A` then `B`; a unit clause uses only `A`.
    std::vector<Word> words_;
    /// @brief Per cell, a copy of its watch, so a rebuild reads no clause; kept only once
    /// a backjump could rebuild the cell: built above the root, or its watch moved.
    std::vector<Word> watch_words_;
    std::vector<std::uint32_t> key_;
    std::vector<std::uint32_t> gen_; ///< Bumped whenever a cell's watch changes.
    std::vector<std::uint32_t> pair_clause_;
    std::vector<std::uint8_t> pair_is_unit_;
    /// @brief `no_pair` for a clause that had none built, or lost it to `free_pairs_`.
    std::vector<std::uint32_t> clause_pair_;
    /// @brief The pairs of the clauses gone inactive before the last substitution, for
    /// `build_pair()`; a bucket may still hold their cells, under a key they have lost.
    std::vector<std::uint32_t> free_pairs_;
    /// @brief The pairs of the clauses gone inactive since, which a save may still name.
    std::vector<std::uint32_t> retired_pairs_;

    std::vector<std::vector<std::uint32_t>> buckets_; ///< Per column, cells that may lead on it.
    std::size_t bucket_entries_ = 0;
    std::size_t cleanup_threshold_ = 0;
    std::vector<std::uint8_t> cleanup_kept_; ///< Per cell, whether this cleanup kept an entry.

    std::vector<const Word*> pivot_row_; ///< Per column, the trail row pivoting on it.
    std::vector<std::uint32_t> pivot_col_idxs_; ///< In trail order, with the constant's omitted.
    std::vector<std::size_t> level_pivot_mark_;
    /// @brief A queue, oldest first, over `[paused_head_, paused_end_)`; the other
    /// slots keep their storage.
    std::vector<PausedSweep> paused_sweeps_;
    std::size_t paused_head_ = 0;
    std::size_t paused_end_ = 0;

    std::vector<SaveEntry> saves_;
    std::vector<Word> save_words_; ///< The saved cells' words are its first `num_save_words_`.
    std::size_t num_save_words_ = 0;
    std::vector<std::size_t> level_save_mark_;
    std::vector<std::size_t> level_save_word_mark_;
    std::vector<std::uint32_t> level_stamp_; ///< Fresh at every opening of a level.
    std::uint32_t next_stamp_ = 1;
    std::vector<std::uint32_t> save_stamp_; ///< Per cell, the stamp of the level it last saved at.
    std::vector<std::uint32_t> restore_stamp_;
    std::vector<std::uint8_t> restore_rebuild_;
    std::uint32_t restore_epoch_ = 0;
    std::vector<std::uint32_t> restored_cells_;
    /// @brief The cells a backjump restores, `A`s then `B`s, each in the order restored:
    /// twice the cell's pair, plus one if the cell is rebuilt.
    std::vector<std::uint32_t> restored_by_kind_[2];
    std::vector<std::uint32_t> rebuilt_cells_;

    /// @brief Triggers found by building a pair rather than by a sweep.
    std::vector<ClauseId> side_queue_;
    std::size_t side_queue_head_ = 0;
    std::vector<std::uint8_t> in_side_queue_;

    ExactWatchlistCounters counters_;

#ifndef NDEBUG
    std::size_t debug_events_ = 0;
    std::vector<std::uint8_t> debug_watch_loaded_;
#endif
};

} // namespace xorcle
