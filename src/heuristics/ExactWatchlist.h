#pragma once

#include <cstddef>
#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "heuristics/ClauseSelector.h"

namespace heuristics {

/// @brief Encoded id for one watched row: watch0, watch1, or watch0+watch1.
class WatchRowId {
private:
    std::size_t raw_;
    static constexpr std::size_t null_value = std::numeric_limits<std::size_t>::max();

public:
    constexpr WatchRowId() noexcept : raw_(null_value) {}
    constexpr WatchRowId(std::size_t raw) noexcept : raw_(raw) {}
    constexpr WatchRowId(ClauseId id, WatchType wt) noexcept
      : raw_(3 * id + static_cast<std::size_t>(wt)) {}

    constexpr ClauseId clause_id() const noexcept { return raw_ / 3; }
    constexpr WatchType watch_type() const noexcept { return static_cast<WatchType>(raw_ % 3); }
    constexpr bool has_value() const noexcept { return raw_ != null_value; }
    constexpr operator std::size_t() const noexcept { return raw_; }

    friend constexpr auto operator<=>(WatchRowId, WatchRowId) = default;
};

/// @brief Exact watchlist selector with lazy row processing and strict semantic checks.
/// @note A watched "row" means `watch0`, `watch1`, or `watch0 + watch1`.
/// Active buckets are keyed by the reduced `first_set()` of that row:
/// variable index, constant bucket (`0=1`), or zero bucket (`0=0`).
/// Deferred tasks may go stale after watch updates or backjumps; dropping those
/// stale tasks is intentional lazy behavior, not invariant repair.
class ExactWatchlist : public ClauseSelector {
private:
    /// @brief Cached reduced form of one watched row at one trail level.
    /// @note `valid == false` means the cache entry may be resized but must not
    /// be read semantically until rebuilt for that `(row, level)` pair.
    struct ReducedCell {
        Equation eqn;
        bool valid = false;
    };

    /// @brief Reference to a watched row together with the version it was queued under.
    /// @note A queued row becomes stale when watch identities change or when a
    /// level-specific task survives a backjump/compaction to a different level.
    struct VersionedRowRef {
        std::size_t row_idx;
        std::uint64_t version;
        std::size_t level = std::numeric_limits<std::size_t>::max();
    };

    enum class DeferredTaskKind : std::uint8_t {
        row_reclassify,
        unit_probe
    };

    struct DeferredTask {
        DeferredTaskKind kind;
        VersionedRowRef row_ref;
        ClauseId clause_id = 0;
    };

    struct ReducedClauseState {
        bool unit = false;
        bool watch0_zero = false;
        bool watch1_zero = false;
        bool sum_zero = false;
        bool watch0_one = false;
        bool watch1_one = false;
        bool sum_one = false;
    };

    static constexpr std::size_t invalid_bucket = std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t invalid_row_idx = std::numeric_limits<std::size_t>::max();

    std::size_t num_vars = 0;
    /// @brief Trail level for which active buckets and queued row tasks are currently interpreted.
    std::size_t current_level = 0;
    /// @brief Current trail used to validate selections and rebuild cached rows.
    /// @note This is null only before the selector has been synchronized with any trail.
    const Trail* current_trail_ptr = nullptr;

    /// @brief Cached reduced watched rows indexed by `[row_id][trail_level]`.
    /// @note All row-indexed vectors in this class share the same row-id domain.
    std::vector<std::vector<ReducedCell>> cells_by_row;
    /// @brief Marks rows that are placeholders for unit or inactive clauses.
    /// @note Placeholder rows never participate in active buckets.
    std::vector<std::uint8_t> row_is_unit_placeholder;
    /// @brief Monotonic row versions used to discard stale deferred row work.
    std::vector<std::uint64_t> row_version;

    /// @brief Bucket linked-list state for the current level.
    /// @note Only rows with a valid current-level reduced cell may appear here.
    std::vector<std::size_t> row_next_in_bucket;
    std::vector<std::size_t> row_prev_in_bucket;
    std::vector<std::size_t> row_bucket;
    std::vector<std::size_t> bucket_head_row;

    /// @brief LIFO deferred work stack.
    /// @note `row_reclassify` tasks rebuild one watched row at the current level.
    /// `unit_probe` tasks re-check a unit clause directly. `push_rows_as_tasks()`
    /// reverses its input so an existing bottom-to-top ordering remains intact.
    std::vector<DeferredTask> work_stack;
    /// @brief Active unit clauses that still need probing when the selector is idle.
    std::vector<ClauseId> unit_clauses;

    /// @brief Scratch equations reused to avoid temporary allocations.
    Equation scratch0;
    Equation scratch1;
    Equation scratch2;

    std::size_t zero_bucket() const { return num_vars + 1; }
    std::size_t const_bucket() const { return num_vars; }
    std::size_t bucket_count() const { return num_vars + 2; }

    /// @brief Record the trail against which cached rows and selections are interpreted.
    /// @param trail Current synchronized trail.
    void set_current_trail(const Trail& trail) { current_trail_ptr = &trail; }

    /// @name Row Cache Helpers
    /// @{
    /// @brief Ensure row-indexed storage exists for one watched-row id.
    void ensure_row_storage(WatchRowId row);
    /// @brief Ensure row-indexed storage exists up to `max_idx`.
    void ensure_row_storage_up_to(std::size_t max_idx);
    /// @brief Return whether `row` is a placeholder for a unit or inactive clause.
    bool row_is_placeholder(WatchRowId row) const;
    /// @brief Return whether the reduced cache cell for `(row, level)` is valid.
    bool cell_is_valid(WatchRowId row, std::size_t level) const;
    /// @brief Build the unreduced watched expression for `row`.
    /// @param row Watched row identity.
    /// @param out Output equation reused by the caller.
    /// @pre `row` belongs to an active non-unit clause.
    void build_base_row_expression(WatchRowId row, Equation& out) const;
    /// @brief Get or build the reduced watched-row cache for `(row, level)`.
    /// @param row Watched row identity.
    /// @param level Trail level whose snapshot is used for reduction.
    /// @param trail Trail providing the level snapshots.
    /// @return The rebuilt cell, or null when the row is intentionally inactive/placeholder.
    /// @note When the previous level cache is valid, the next level reduction starts from it.
    ReducedCell* get_or_build_cell(WatchRowId row, std::size_t level, const Trail& trail);
    /// @brief Invalidate all cached levels for one watched row.
    void invalidate_row_cache(WatchRowId row);
    /// @brief Recompute one watched row immediately and reinsert it into the current bucket state.
    /// @return The row's current bucket, or `invalid_bucket` only for placeholder/inactive rows.
    std::size_t recompute_watched_row_now(WatchRowId row, const Trail& trail);
    /// @}

    /// @name Bucket Helpers
    /// @{
    /// @brief Classify a reduced `first_set()` into variable, constant, or zero bucket space.
    std::size_t classify_bucket_from_first_set(std::size_t first_set) const;
    /// @brief Classify a reduced equation into its current active bucket.
    std::size_t classify_bucket(const Equation& eqn) const;
    /// @brief Clear all active-bucket linkage without touching cached row cells.
    void clear_active_buckets();
    /// @brief Remove a row from the active bucket lists.
    /// @pre `row` belongs to the current row-storage domain.
    void remove_row_from_active_bucket(WatchRowId row);
    /// @brief Insert a row at the head of `bucket`.
    /// @pre `bucket < bucket_count()`.
    void insert_row_into_active_bucket(WatchRowId row, std::size_t bucket);
    /// @brief Invalidate cached cells strictly above `level`.
    void invalidate_levels_above(std::size_t level);
    /// @brief Rebuild active buckets from already-valid current-level cached cells.
    void rebuild_active_buckets_from_valid_cells(const Trail& trail);
    /// @brief Detach and return every row currently stored in `bucket`.
    std::vector<VersionedRowRef> detach_bucket_rows(std::size_t bucket);
    /// @brief Snapshot the rows that are currently in the zero bucket.
    std::vector<VersionedRowRef> snapshot_zero_bucket_rows() const;
    /// @brief Collect active non-placeholder rows missing a current-level cache cell.
    std::vector<VersionedRowRef> collect_rows_missing_current_level_cell() const;
    /// @}

    /// @name Deferred Task Helpers
    /// @{
    /// @brief Queue row reclassification for `row` using its current version.
    void push_row_task(WatchRowId row);
    /// @brief Queue row reclassification for an already-versioned row ref.
    void push_row_task(VersionedRowRef ref);
    /// @brief Queue a row task that is valid only for `current_level`.
    void push_row_task_for_current_level(WatchRowId row);
    /// @brief Push many row tasks while preserving the caller's bottom-to-top priority.
    void push_rows_as_tasks(std::vector<VersionedRowRef>&& rows);
    /// @brief Queue a unit-clause probe.
    void push_unit_task(ClauseId id);
    /// @brief Return whether a queued row reference is stale under current versions/level.
    bool row_ref_is_stale(const VersionedRowRef& ref) const;
    /// @brief Process one deferred task and maybe produce an actionable selection.
    std::optional<ClauseSelection> process_one_deferred_task();
    /// @}

    /// @name Selection Semantics Helpers
    /// @{
    /// @brief Append one clause and initialize all row-indexed selector state for it.
    /// @param clause Clause being stored.
    /// @param queue_unit_probe Whether a newly added unit clause should be queued immediately.
    void append_clause_rows(Clause clause, bool queue_unit_probe);
    /// @brief Advance `current_level` by one if the trail grew incrementally.
    /// @throws std::logic_error if the trail level jump is not exactly one.
    void apply_incremental_level_change(const Trail& trail);
    /// @brief Reduce the watched rows of one clause against `trail_or_null`.
    /// @note `trail_or_null == nullptr` means "evaluate in base form" and is only used
    /// where the current semantic check does not require reduction by a live trail.
    ReducedClauseState reduce_clause_watches_now(ClauseId id, const Trail* trail_or_null);
    /// @brief Return whether the watched expression named by `wt` currently reduces to `0 = 0`.
    bool trigger_is_zero(WatchType wt, const ReducedClauseState& state) const;
    /// @brief Return whether a zero trigger currently makes the clause actionable.
    /// @throws std::logic_error if the watched rows violate the selector/propagator trigger identities.
    bool trigger_allows_action(ClauseId id, WatchType wt, const ReducedClauseState& state) const;
    /// @brief Try to select a non-unit clause from one watched zero trigger.
    /// @param require_zero_now When true, a non-zero trigger is treated as invariant failure.
    std::optional<ClauseSelection> try_select_from_trigger(ClauseId id,
                                                           WatchType wt,
                                                           const Trail* trail_or_null,
                                                           bool require_zero_now);
    /// @brief Try to select a unit clause whose reduced row is not `0 = 1`.
    std::optional<ClauseSelection> try_select_unit_clause(ClauseId id, const Trail* trail_or_null);
    /// @brief Validate that a returned selection is immediately actionable for the current semantics.
    void validate_selection_or_throw(const ClauseSelection& selection, const Trail* trail_or_null);
    /// @brief Throw a semantic invariant failure with selector context.
    [[noreturn]] void throw_semantic_error(const char* where, const std::string& msg) const;
    /// @}

#ifndef NDEBUG
    /// @brief Assert that all row-indexed vectors cover the same row-id domain.
    void debug_assert_row_storage_domains() const;
    /// @brief Assert that a non-unit clause has valid distinct watched rows and matching width.
    void debug_assert_active_non_unit_clause_valid(ClauseId id) const;
    /// @brief Assert that bucket linked lists are structurally valid for the current level.
    void debug_assert_bucket_links() const;
    void debug_assert_active_buckets_match_cells(const Trail& trail) const;
    void debug_assert_no_actionable_clause_when_idle(const Trail& trail);
#endif

public:
    explicit ExactWatchlist(std::size_t num_vars);
    ~ExactWatchlist() override;

    std::optional<ClauseId> next() override;
    /// @brief Return the next actionable clause selection.
    /// @return A selection whose hint is immediately actionable for the current trail.
    /// @pre The selector has been kept synchronized with the current trail through
    /// `add_asserting_clause`, `update_with_new_equation`, `update_with_decision`,
    /// and `update_with_new_watch`.
    std::optional<ClauseSelection> next_with_hint() override;
    void add_starting_clause(Clause clause) override;
    /// @brief Add an asserting clause after a backjump.
    /// @param clause The learned asserting clause.
    /// @param trail The backjumped trail state.
    /// @param jumpable_levels Number of removed decision levels.
    /// @pre `trail` already reflects the post-backjump state.
    /// @note This rebuilds deferred work against the current trail level instead of
    /// trying to repair pre-backjump row tasks.
    void add_asserting_clause(Clause clause, const Trail& trail, std::size_t jumpable_levels) override;
    /// @brief Notify the selector that a learned equation extended the trail by one level.
    /// @param eqn Newly learned reduced equation.
    /// @param trail Current trail including that equation.
    /// @pre `trail.size() == current_level + 1` before the update.
    void update_with_new_equation(const Equation& eqn, const Trail& trail) override;
    void update_with_decision(const Equation& reduced_eqn, const Trail& trail) override;
    /// @brief Refresh cached rows after a watched row changes.
    /// @param id Clause whose watch changed.
    /// @param old_watch Previously watched row index.
    /// @param wt Which watch changed.
    /// @param trail Current trail.
    /// @pre `wt != WatchType::Sum`.
    void update_with_new_watch(ClauseId id, EquationId old_watch, WatchType wt, const Trail& trail) override;
    void update_with_compaction(const search::CompactionTransform& transform, const Trail& trail) override;
};

} // namespace heuristics
