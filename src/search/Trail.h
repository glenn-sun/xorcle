#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "algebra/Equation.h"
#include "algebra/RowSpace.h"
#include "heuristics/Types.h"

namespace search {

/// @brief A snapshot of all learned equations up to and including one decision level.
///
/// @note Representation invariants:
/// - `span`, `order_map`, `reverse_order_map`, and `reasons` describe the same learned rows
/// - `order_map` maps trail insertion order to row indices in `span`
/// - `reverse_order_map` is the inverse permutation of `order_map`
/// - `reasons[i]` is the reason for the row inserted at trail order `i`
class TrailSnapshot {
public:
    /// @brief Map from trail insertion order to the row in representation.
    std::vector<std::size_t> order_map;

    /// @brief Map from row in representation to trail insertion order.
    std::vector<std::size_t> reverse_order_map;

    /// @brief Space of known true equations.
    algebra::RowSpace span;

    /// @brief Map from trail insertion order to reason.
    std::vector<std::optional<heuristics::ClauseId>> reasons;

    /// @brief Construct an empty snapshot for a given variable count.
    /// @param num_vars Number of variable columns. The constant column is added internally.
    explicit TrailSnapshot(std::size_t num_vars) : span(num_vars) {}

    /// @brief Learn a new equation into this snapshot.
    /// @param eqn The equation to learn.
    /// @param reason The reason for learning eqn, or std::nullopt if eqn is a decision.
    /// @pre `eqn.size() == span.cols()`.
    /// @pre `eqn` is reduced by the current trail span before learning.
    /// @pre If `reason` is provided, then `eqn + 1` belongs to that reason clause.
    void learn(const algebra::Equation& eqn, std::optional<heuristics::ClauseId> reason);
};

/// @brief The solver trail, stored as immutable snapshots by decision level.
///
/// @note `back()` is the live trail state. `trail[k]` stores all learned equations at
/// levels `<= k`. Every non-root snapshot begins with that level's decision equation.
class Trail : public std::vector<TrailSnapshot> {
public:
    /// @brief Construct a trail with an initial empty snapshot.
    /// @param num_vars Number of variable columns. The constant column is added internally.
    explicit Trail(std::size_t num_vars) : std::vector<TrailSnapshot>() {
        push_back(TrailSnapshot(num_vars));
    }

    /// @brief Learn a new equation, creating a new decision level when reason is null.
    /// @param eqn The equation to learn.
    /// @param reason The reason for learning eqn, or std::nullopt if eqn is a decision.
    /// @pre `eqn.size() == span().cols()`.
    /// @pre `eqn` is reduced by the current trail span before learning.
    /// @pre If `reason` is provided, then `eqn + 1` belongs to that reason clause.
    void learn(const algebra::Equation& eqn, std::optional<heuristics::ClauseId> reason);

    /// @brief The space of known true equations.
    const algebra::RowSpace& span() const {
        return back().span;
    }

    /// @brief The map from trail insertion order to the row in representation.
    const std::vector<std::size_t>& order_map() const {
        return back().order_map;
    }

    /// @brief The map from row in representation to trail insertion order.
    const std::vector<std::size_t>& reverse_order_map() const {
        return back().reverse_order_map;
    }

    /// @brief The map from trail insertion order to reason.
    const std::vector<std::optional<heuristics::ClauseId>>& reasons() const {
        return back().reasons;
    }
};
} // namespace search
