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

enum class PendingDedupPolicy {
    dedup,
    allow_duplicates
};

struct ExactWatchlist2Config {
    PendingDiscipline discipline = PendingDiscipline::lifo_stack;
    PendingDedupPolicy dedup = PendingDedupPolicy::dedup;
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
        std::size_t epoch = 0;
        std::uint64_t level_stamp = 0;
        bool computed = false;
    };

    static constexpr std::size_t invalid_bucket = std::numeric_limits<std::size_t>::max();

    std::size_t num_vars;
    ExactWatchlist2Config config;
    std::size_t current_level = 0;
    std::vector<std::uint64_t> level_stamp_by_index;
    std::uint64_t next_level_stamp = 1;

    // Row-wise cache indexed by WatchedId then trail level.
    std::vector<std::vector<CacheCell>> cells_by_row;
    std::vector<std::uint64_t> row_epoch;
    std::vector<bool> row_structural_placeholder;

    // Active current-level bucket lists.
    std::vector<EW2WatchedId> active_next;
    std::vector<EW2WatchedId> active_prev;
    std::vector<std::size_t> active_bucket;
    std::vector<EW2WatchedId> bucket_head;

    // Pending clause scheduler.
    std::deque<ClauseId> pending;
    std::vector<bool> pending_set;
    std::vector<ClauseId> unit_clauses;

    Equation working_eqn;

    std::size_t zero_bucket() const { return num_vars + 1; }
    std::size_t const_bucket() const { return num_vars; }
    std::size_t num_buckets() const { return num_vars + 2; }

    bool dedup_enabled() const { return config.dedup == PendingDedupPolicy::dedup; }

    void push_pending(ClauseId id);
    ClauseId pop_pending();

    void append_clause_rows(const Clause& clause, bool enqueue_if_unit);
    void ensure_row_capacity(EW2WatchedId wid);

    bool row_is_placeholder(EW2WatchedId wid) const;
    bool cell_is_valid(EW2WatchedId wid, std::size_t level) const;
    CacheCell* ensure_cell(EW2WatchedId wid, std::size_t level, const Trail& trail);

    void partial_reduce_in_place(Equation& eqn, const algebra::RowSpace& span) const;
    void build_base_expression(EW2WatchedId wid, Equation& out) const;

    std::size_t classify_bucket(const Equation& eqn) const;

    void clear_active_buckets();
    void insert_active(EW2WatchedId wid, std::size_t bucket);
    void remove_active(EW2WatchedId wid);
    void rebuild_active_from_level(const Trail& trail);
    void sync_current_level_rebuild_only(const Trail& trail);
    void seed_pending_from_zero_bucket();
    void on_level_change_incremental(const Trail& trail);

    void invalidate_row(EW2WatchedId wid);
    void refresh_watched_row_now(EW2WatchedId wid, const Trail& trail);

// Expensive O(num_clauses) invariant checks are opt-in even in debug builds.
// Define PARITY_ENABLE_EXPENSIVE_SELECTOR_CHECKS to enable them.
#if defined(PARITY_ENABLE_EXPENSIVE_SELECTOR_CHECKS)
    void assert_active_membership_matches_current_level(const Trail& trail) const;
#endif

public:
    explicit ExactWatchlist2(std::size_t num_vars, ExactWatchlist2Config config = {});

    std::optional<ClauseId> next() override;
    void add_starting_clause(const Clause& clause) override;
    void add_asserting_clause(const Clause& clause, const Trail& trail, std::size_t jumpable_levels) override;
    void update_with_new_equation(const Equation& eqn, const Trail& trail) override;
    void update_with_decision(const Equation& reduced_eqn, const Trail& trail) override;
    void update_with_new_watch(ClauseId id, EquationId old_watch, WatchType wt, const Trail& trail) override;
};

} // namespace heuristics
