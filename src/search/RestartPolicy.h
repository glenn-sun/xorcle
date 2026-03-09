#pragma once

#include <cstddef>

namespace search {

/// @brief Supported solver restart scheduling methods.
enum class RestartMethod {
    none,      ///< Disable restarts.
    geometric, ///< Geometric conflict-budget growth.
    luby       ///< Luby-sequence conflict budgets.
};

/// @brief Runtime restart configuration passed into the solver.
struct RestartConfig {
    RestartMethod method = RestartMethod::none;
    double geometric_multiplier = 1.5;
    std::size_t geometric_initial_conflicts = 100;
    std::size_t luby_unit_conflicts = 100;
};

/// @brief Stateful restart scheduler driven by conflict counts.
class RestartPolicy {
public:
    /// @brief Construct from a resolved restart configuration.
    explicit RestartPolicy(const RestartConfig& config = {});

    /// @brief Return true if restarts are enabled.
    bool enabled() const;

    /// @brief Record one non-root conflict and report whether to restart now.
    /// @return True exactly when the current conflict exhausts the active restart budget.
    /// @note This consumes a conflict from the current budget, but does not advance to
    /// the next budget. Call `note_restart()` only after an actual restart happens.
    bool should_restart_after_conflict();

    /// @brief Advance to the next restart budget after an actual restart.
    /// @note This must be called only after the solver has really restarted.
    void note_restart();

    /// @brief Return the 1-indexed Luby sequence value.
    static std::size_t luby_value(std::size_t index);

private:
    RestartConfig config_;
    std::size_t conflicts_since_restart_ = 0;
    std::size_t current_budget_ = 0;
    std::size_t luby_index_ = 1;

    /// @brief Recompute the active conflict budget from current state.
    void refresh_budget();
};

} // namespace search
