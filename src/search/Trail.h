#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "algebra/Equation.h"
#include "algebra/RowSpace.h"
#include "heuristics/Types.h"

namespace search {

/// @brief A snapshot of all entries at level <= some k in the trail.
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
    explicit TrailSnapshot(std::size_t num_vars) : span(num_vars) {}

    /// @brief Learn a new equation into this snapshot.
    ///
    /// @pre If reason is provided, then eqn + 1 belongs to the span of that reason.
    /// @note eqn is reduced by the current trail span before learning.
    /// @param eqn The equation to learn.
    /// @param reason The reason for learning eqn, or std::nullopt if eqn is a decision.
    void learn(const algebra::Equation& eqn, std::optional<heuristics::ClauseId> reason);
};


/// @brief The trail of known true equations is stored in back(). Each intermediate snapshot
/// is a point in time that is can be backjumped to. trail[k] stores all true equations at
/// level <= k, and all snapshots except the first contain a decision as the first equation.
class Trail : public std::vector<TrailSnapshot> {
public:
    /// @brief Construct a trail with an initial empty snapshot.
    explicit Trail(std::size_t num_vars) : std::vector<TrailSnapshot>() {
        push_back(TrailSnapshot(num_vars));
    }

    /// @brief Learn a new equation, creating a new decision level when reason is null.
    ///
    /// @pre If reason is provided, then eqn + 1 belongs to the span of that reason.
    /// @note eqn is reduced by the current trail span before learning.
    /// @param eqn The equation to learn.
    /// @param reason The reason for learning eqn, or std::nullopt if eqn is a decision.
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
