#pragma once

#include "RowSpace.h"
#include "Equation.h"

/// @brief A snapshot of all entries at level <= some k in the trail.
class TrailSnapshot {
public:
    // Map from trail insertion order to the row in representation
    std::vector<std::size_t> order_map;

    // Map from row in representation to trail insertion order
    std::vector<std::size_t> reverse_order_map;

    // Space of known true equations
    RowSpace span;

    // Map from trail insertion order to reason
    std::vector<std::optional<std::reference_wrapper<Clause>>> reasons;

    /// @brief Learn a new equation. Requires that eqn + 1 belongs to the span of reason.
    /// eqn will be reduced by the current trail span before learning.
    /// @param eqn The equation to learn
    /// @param reason The reason for learning eqn, or std::nullopt if eqn is a decision
    void learn(const Equation& eqn, std::optional<std::reference_wrapper<Clause>> reason);
};


/// @brief The trail of known true equations is stored in back(). Each intermediate snapshot
/// is a point in time that is can be backjumped to. trail[k] stores all true equations at
/// level <= k, and all snapshots except the first contain a decision as the first equation.
class Trail : public std::vector<TrailSnapshot> {
public:
    Trail() : std::vector<TrailSnapshot>() {
        push_back(TrailSnapshot());
    }

    /// @brief Learn a new equation. Requires that eqn + 1 belongs to the span of reason.
    /// eqn will be reduced by the current trail span before learning. Creates a new snapshot
    /// if eqn is a decision.
    /// @param eqn The equation to learn
    /// @param reason The reason for learning eqn, or std::nullopt if eqn is a decision
    void learn(const Equation& eqn, std::optional<std::reference_wrapper<Clause>> reason) {
        if (!reason.has_value()) {
            TrailSnapshot new_snapshot = back();
            push_back(new_snapshot);
        }
        back().learn(eqn, reason);
    }

    /// @brief The space of known true equations
    const RowSpace& span() const { 
        return back().span; 
    }

    /// @brief The map from trail insertion order to the row in representation
    const std::vector<std::size_t>& order_map() const {
        return back().order_map;
    }

    /// @brief The map from row in representation to trail insertion order
    const std::vector<std::size_t>& reverse_order_map() const {
        return back().reverse_order_map;
    }

    /// @brief The map from trail insertion order to reason
    const std::vector<std::optional<std::reference_wrapper<Clause>>>& reasons() const {
        return back().reasons;
    }
};