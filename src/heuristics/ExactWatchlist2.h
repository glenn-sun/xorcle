#pragma once

#include <cstddef>
#include <compare>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <vector>

#include "heuristics/ClauseSelector.h"

namespace heuristics {

enum class PendingDiscipline {
    lifo_stack,
    fifo_queue
};

struct ExactWatchlist2Config {
    PendingDiscipline discipline = PendingDiscipline::fifo_queue;
};

/// @brief Encoded identifier for a watched expression row in ExactWatchlist2.
class EW2WatchedId {
private:
    std::size_t raw_;
    static constexpr std::size_t null_value = std::numeric_limits<std::size_t>::max();

public:
    constexpr EW2WatchedId() noexcept : raw_(null_value) {}
    constexpr EW2WatchedId(std::size_t raw) noexcept : raw_(raw) {}
    constexpr EW2WatchedId(ClauseId id, WatchType wt) noexcept
      : raw_(3 * id + static_cast<std::size_t>(wt)) {}

    constexpr ClauseId clauseId() const noexcept { return raw_ / 3; }
    constexpr WatchType watchType() const noexcept { return static_cast<WatchType>(raw_ % 3); }
    constexpr bool has_value() const noexcept { return raw_ != null_value; }
    constexpr operator std::size_t() const noexcept { return raw_; }

    friend constexpr auto operator<=>(EW2WatchedId, EW2WatchedId) = default;
};

class ExactWatchlist2 : public ClauseSelector {
private:
    struct CacheCell {
        Equation eqn;
        bool valid = false;
    };

    static constexpr std::size_t invalid_bucket = std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t invalid_wid_idx = std::numeric_limits<std::size_t>::max();

    struct DeferredRowRef {
        std::size_t row_idx;
        std::uint64_t version;
    };

    enum class DeferredWorkKind : std::uint8_t {
        bucket_reassign,
        zero_reseed
    };

    struct DeferredWorkItem {
        DeferredWorkKind kind;
        std::vector<DeferredRowRef> rows;
        std::size_t pos = 0;
    };

    std::size_t num_vars;
    ExactWatchlist2Config config;
    std::size_t current_level = 0;
    const Trail* current_trail_ptr = nullptr;

    // Row-wise cache indexed by WatchedId then trail level.
    std::vector<std::vector<CacheCell>> cells_by_row;
    std::vector<std::uint8_t> row_structural_placeholder;
    std::vector<std::uint64_t> row_work_version;

    // Active current-level bucket lists.
    std::vector<std::size_t> active_next_idx;
    std::vector<std::size_t> active_prev_idx;
    std::vector<std::size_t> active_bucket;
    std::vector<std::size_t> bucket_head_idx;

    // Pending clause scheduler.
    std::deque<ClauseSelection> pending;
    std::vector<std::uint8_t> pending_unit_set;
    std::vector<std::uint8_t> pending_watch_set;
    std::deque<DeferredWorkItem> deferred_work;
    std::vector<ClauseId> unit_clauses;

    // Debug counters: queue entries enqueued vs popped vs discarded on conflict clears.
    std::size_t debug_pending_enqueued = 0;
    std::size_t debug_pending_popped = 0;
    std::size_t debug_pending_discarded_on_conflict_clear = 0;
    std::size_t debug_conflict_clears = 0;
    std::size_t debug_deferred_bucket_scans_created = 0;
    std::size_t debug_deferred_reseed_scans_created = 0;
    std::size_t debug_deferred_rows_processed = 0;
    std::size_t debug_deferred_rows_skipped_stale = 0;
    std::size_t debug_deferred_rows_skipped_not_zero = 0;

    Equation working_eqn;
    Equation working_eqn2;

    std::size_t zero_bucket() const { return num_vars + 1; }
    std::size_t const_bucket() const { return num_vars; }
    std::size_t num_buckets() const { return num_vars + 2; }

    WatchType normalize_nonunit_hint(WatchType wt) const;
    std::size_t watch_pending_key(ClauseId id, WatchType wt) const;
    void push_pending_unit(ClauseId id);
    void push_pending_watch(ClauseId id, WatchType wt);
    void push_pending_from_zero_row(EW2WatchedId wid);
    ClauseSelection pop_pending();

    void append_clause_rows(const Clause& clause, bool enqueue_if_unit);
    void ensure_row_capacity(EW2WatchedId wid);
    void ensure_row_capacity_up_to(std::size_t max_idx);

    bool row_is_placeholder(EW2WatchedId wid) const;
    bool cell_is_valid(EW2WatchedId wid, std::size_t level) const;
    CacheCell* ensure_cell(EW2WatchedId wid, std::size_t level, const Trail& trail);
    CacheCell* ensure_cell_existing_row(EW2WatchedId wid, std::size_t level, const Trail& trail);
    void set_current_trail(const Trail& trail) { current_trail_ptr = &trail; }

    void build_base_expression(EW2WatchedId wid, Equation& out) const;

    std::size_t classify_bucket_from_first_set(std::size_t f) const;
    std::size_t classify_bucket(const Equation& eqn) const;

    void clear_active_buckets();
    void insert_active(EW2WatchedId wid, std::size_t bucket);
    void remove_active(EW2WatchedId wid);
    void sync_current_level_rebuild_only(const Trail& trail);
    void discard_levels_after(std::size_t level);
    void seed_pending_from_zero_bucket();
    void on_level_change_incremental(const Trail& trail);
    std::vector<DeferredRowRef> detach_bucket_to_snapshot(std::size_t bucket);
    std::vector<DeferredRowRef> snapshot_zero_bucket_rows() const;
    void push_deferred_bucket_scan(std::vector<DeferredRowRef>&& rows);
    void push_deferred_zero_reseed(std::vector<DeferredRowRef>&& rows);
    bool step_one_deferred_work();

    void invalidate_row(EW2WatchedId wid);
    void refresh_watched_row_now(EW2WatchedId wid, const Trail& trail);
    void enqueue_clause_watch_tasks_now(ClauseId id, const Trail& trail);

// Expensive O(num_clauses) invariant checks are opt-in even in debug builds.
// Define PARITY_ENABLE_EXPENSIVE_SELECTOR_CHECKS to enable them.
#if defined(PARITY_ENABLE_EXPENSIVE_SELECTOR_CHECKS)
    void assert_active_membership_matches_current_level(const Trail& trail) const;
#endif

public:
    explicit ExactWatchlist2(std::size_t num_vars, ExactWatchlist2Config config = {});
    ~ExactWatchlist2() override;

    std::optional<ClauseId> next() override;
    std::optional<ClauseSelection> next_with_hint() override;
    void add_starting_clause(const Clause& clause) override;
    void add_asserting_clause(const Clause& clause, const Trail& trail, std::size_t jumpable_levels) override;
    void update_with_new_equation(const Equation& eqn, const Trail& trail) override;
    void update_with_decision(const Equation& reduced_eqn, const Trail& trail) override;
    void update_with_new_watch(ClauseId id, EquationId old_watch, WatchType wt, const Trail& trail) override;
};

} // namespace heuristics
