#pragma once

#include <cstddef>
#include <compare>
#include <limits>
#include <optional>
#include <vector>

#include "heuristics/ClauseSelector.h"
#include "search/Trail.h"

namespace heuristics {

/// @brief Encoded identifier for a watched equation in a clause.
class WatchedId {
private:
    /// @brief Packed value (3 * clause_id + watch_type), or null_value.
    std::size_t raw_;

    /// @brief Sentinel for an invalid watch id.
    static constexpr std::size_t null_value = std::numeric_limits<std::size_t>::max();

public:
    /// @brief Construct an invalid watch id.
    constexpr WatchedId() noexcept : raw_(null_value) {}

    /// @brief Construct from a raw packed value.
    constexpr WatchedId(std::size_t raw) noexcept : raw_(raw) {}

    /// @brief Construct from a clause id and watch type.
    constexpr WatchedId(ClauseId id, WatchType wt) noexcept
      : raw_(3 * id + static_cast<std::size_t>(wt)) {}

    /// @brief Return the clause id encoded by this watch.
    constexpr ClauseId clauseId() const noexcept { return raw_ / 3; }

    /// @brief Return the watch type encoded by this watch.
    constexpr WatchType watchType() const noexcept { return static_cast<WatchType>(raw_ % 3); }

    /// @brief Return true if this watch id is valid.
    constexpr bool has_value() const noexcept { return raw_ != null_value; }

    /// @brief Implicit conversion to size_t for indexing.
    constexpr operator std::size_t() const noexcept { return raw_; }

    /// @brief Default comparison for ordering and equality.
    friend constexpr auto operator<=>(WatchedId, WatchedId) = default;
};

/// @brief Per-trail snapshot state for the exact watchlist selector.
class ExactWatchlistSnapshot {
public:
    /// @brief Reduced copy of each watched equation; index by WatchedId.
    std::vector<Equation> equations;

    /// @brief Next pointer per watched equation in its bucket list.
    std::vector<WatchedId> bucket_next;

    /// @brief Previous pointer per watched equation in its bucket list.
    std::vector<WatchedId> bucket_prev;

    /// @brief Head pointer for each bucket (variable index to watch list head).
    std::vector<WatchedId> bucket_head;

    /// @brief Map from WatchedId to its current bucket.
    std::vector<std::size_t> which_bucket;

    /// @brief Construct snapshot state for a given number of variables.
    explicit ExactWatchlistSnapshot(std::size_t num_vars);

    /// @brief Add a watched equation to the head of a bucket.
    void add_to_bucket(WatchedId id, std::size_t bucket);

    /// @brief Move a watched equation to a new bucket.
    void move_to_bucket(WatchedId id, std::size_t new_bucket);

    /// @brief Initialize a new watched equation in the specified bucket.
    void init_in_bucket(const Equation& eqn, std::size_t bucket);

    /// @brief Recompute all buckets from scratch for the current trail.
    void recompute_buckets(const std::vector<Clause>& clauses, const Trail& trail, std::size_t num_vars);
};

/// @brief Exact watchlist selector that tracks reduced watched equations per variable.
class ExactWatchlist : public ClauseSelector {
private:
    /// @brief Number of variables in the problem.
    std::size_t num_vars;

    /// @brief Per-level snapshots of watchlist state.
    std::vector<ExactWatchlistSnapshot> snapshots;

    /// @brief Unit clauses tracked separately for fast queueing.
    std::vector<ClauseId> unit_clauses;

    /// @brief Stack of pending clause ids.
    std::vector<ClauseId> stack;

    /// @brief Pop the last queued clause id.
    ClauseId get_pop();

    /// @brief Reusable buffer for temporary equation reductions.
    Equation working_eqn;

public:
    /// @brief Construct an exact watchlist selector for a problem size.
    explicit ExactWatchlist(std::size_t num_vars);

    /// @brief Return the next queued clause id.
    std::optional<ClauseId> next() override;

    /// @brief Add a starting clause and initialize watchers.
    void add_starting_clause(const Clause& clause) override;

    /// @brief Add an asserting clause and refresh buckets.
    void add_asserting_clause(const Clause& clause, const Trail& trail, std::size_t jumpable_levels) override;

    /// @brief Update buckets when a new equation is learned.
    void update_with_new_equation(const Equation& eqn, const Trail& trail) override;

    /// @brief Treat decisions like new equations.
    void update_with_decision(const Equation& reduced_eqn, const Trail& trail) override;

    /// @brief Update internal buckets when a watch is moved.
    void update_with_new_watch(ClauseId id, EquationId old_watch, WatchType wt, const Trail& trail) override;
};
} // namespace heuristics
