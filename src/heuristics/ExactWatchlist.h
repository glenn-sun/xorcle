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
///
/// Jargon:
/// - "row": one watched expression from a clause (watch0, watch1, or watch0+watch1).
/// - "bucket": linked list of rows keyed by reduced first_set():
///   variable index, const bucket (0=1), or zero bucket (0=0).
class ExactWatchlist : public ClauseSelector {
private:
    struct ReducedCell {
        Equation eqn;
        bool valid = false;
    };

    struct VersionedRowRef {
        std::size_t row_idx;
        std::uint64_t version;
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
    std::size_t current_level = 0;
    const Trail* current_trail_ptr = nullptr;

    // Cached reduced rows indexed by [row_id][trail_level].
    std::vector<std::vector<ReducedCell>> cells_by_row;
    std::vector<std::uint8_t> row_is_unit_placeholder;
    std::vector<std::uint64_t> row_version;

    // Active bucket linked lists at current level.
    std::vector<std::size_t> row_next_in_bucket;
    std::vector<std::size_t> row_prev_in_bucket;
    std::vector<std::size_t> row_bucket;
    std::vector<std::size_t> bucket_head_row;

    // LIFO lazy deferred-task stack.
    std::vector<DeferredTask> work_stack;
    std::vector<ClauseId> unit_clauses;

    // Scratch equations reused to avoid temporary allocations.
    Equation scratch0;
    Equation scratch1;
    Equation scratch2;

    std::size_t zero_bucket() const { return num_vars + 1; }
    std::size_t const_bucket() const { return num_vars; }
    std::size_t bucket_count() const { return num_vars + 2; }

    void set_current_trail(const Trail& trail) { current_trail_ptr = &trail; }

    // Row cache and reduction helpers.
    void ensure_row_storage(WatchRowId row);
    void ensure_row_storage_up_to(std::size_t max_idx);
    bool row_is_placeholder(WatchRowId row) const;
    bool cell_is_valid(WatchRowId row, std::size_t level) const;
    void build_base_row_expression(WatchRowId row, Equation& out) const;
    ReducedCell* get_or_build_cell(WatchRowId row, std::size_t level, const Trail& trail);
    void invalidate_row_cache(WatchRowId row);
    std::size_t recompute_watched_row_now(WatchRowId row, const Trail& trail);

    // Bucket helpers.
    std::size_t classify_bucket_from_first_set(std::size_t first_set) const;
    std::size_t classify_bucket(const Equation& eqn) const;
    void clear_active_buckets();
    void remove_row_from_active_bucket(WatchRowId row);
    void insert_row_into_active_bucket(WatchRowId row, std::size_t bucket);
    void invalidate_levels_above(std::size_t level);
    void rebuild_active_buckets_from_valid_cells(const Trail& trail);
    std::vector<VersionedRowRef> detach_bucket_rows(std::size_t bucket);
    std::vector<VersionedRowRef> snapshot_zero_bucket_rows() const;
    std::vector<VersionedRowRef> collect_rows_missing_current_level_cell() const;

    // Deferred-task helpers.
    void push_row_task(WatchRowId row);
    void push_row_task(VersionedRowRef ref);
    void push_rows_as_tasks(std::vector<VersionedRowRef>&& rows);
    void push_unit_task(ClauseId id);
    bool row_ref_is_stale(const VersionedRowRef& ref) const;
    std::optional<ClauseSelection> process_one_deferred_task();

    // Selection semantics helpers.
    void append_clause_rows(const Clause& clause, bool queue_unit_probe);
    void apply_incremental_level_change(const Trail& trail);
    ReducedClauseState reduce_clause_watches_now(ClauseId id, const Trail* trail_or_null);
    bool trigger_is_zero(WatchType wt, const ReducedClauseState& state) const;
    bool trigger_allows_action(ClauseId id, WatchType wt, const ReducedClauseState& state) const;
    std::optional<ClauseSelection> try_select_from_trigger(ClauseId id,
                                                           WatchType wt,
                                                           const Trail* trail_or_null,
                                                           bool require_zero_now);
    std::optional<ClauseSelection> try_select_unit_clause(ClauseId id, const Trail* trail_or_null);
    void validate_selection_or_throw(const ClauseSelection& selection, const Trail* trail_or_null);
    [[noreturn]] void throw_semantic_error(const char* where, const std::string& msg) const;

#ifndef NDEBUG
    void debug_assert_active_buckets_match_cells(const Trail& trail) const;
    void debug_assert_no_actionable_clause_when_idle(const Trail& trail);
#endif

public:
    explicit ExactWatchlist(std::size_t num_vars);
    ~ExactWatchlist() override;

    std::optional<ClauseId> next() override;
    std::optional<ClauseSelection> next_with_hint() override;
    void add_starting_clause(const Clause& clause) override;
    void add_asserting_clause(const Clause& clause, const Trail& trail, std::size_t jumpable_levels) override;
    void update_with_new_equation(const Equation& eqn, const Trail& trail) override;
    void update_with_decision(const Equation& reduced_eqn, const Trail& trail) override;
    void update_with_new_watch(ClauseId id, EquationId old_watch, WatchType wt, const Trail& trail) override;
};

} // namespace heuristics
