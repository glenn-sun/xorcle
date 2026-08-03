#pragma once

/// @file geometric.h
/// @brief The geometric restart schedule.

#include <cstddef>

#include "heuristics/restarts/restart_policy.h"

namespace xorcle {

/// @brief Restart on a geometrically growing conflict budget.
///
/// The first budget is `RestartConfig::geometric_initial_conflicts`; each
/// restart multiplies it by `RestartConfig::geometric_multiplier` and rounds
/// up. With the defaults (100, 1.5) the budgets run
/// 100, 150, 225, 338, 507, 761, ...
///
/// Budgets saturate at `SIZE_MAX` rather than overflowing, and never fall below
/// 1, so the schedule is well defined for any finite multiplier.
///
/// @note Experimental; see `RestartPolicy`.
class Geometric final : public RestartPolicy {
public:
    /// @brief Construct from a resolved restart configuration.
    /// @param config Restart configuration; only the `geometric_*` fields are read.
    explicit Geometric(const RestartConfig& config = {});

    /// @brief Always true.
    bool enabled() const override;

    /// @copydoc RestartPolicy::should_restart_after_conflict
    bool should_restart_after_conflict() override;

    /// @copydoc RestartPolicy::note_restart
    void note_restart() override;

    /// @brief The conflict budget currently being counted down.
    std::size_t current_budget() const { return current_budget_; }

private:
    double multiplier_;                     ///< Growth factor between budgets.
    std::size_t initial_conflicts_;         ///< First budget, before any growth.
    std::size_t conflicts_since_restart_ = 0; ///< Conflicts counted against the active budget.
    std::size_t current_budget_ = 0;        ///< The active budget; 0 only before the first refresh.

    /// @brief Recompute the active conflict budget from current state.
    void refresh_budget();
};

} // namespace xorcle
