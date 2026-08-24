#pragma once

/// @file geometric.h
/// @brief The geometric restart schedule.

#include <cstddef>

#include "heuristics/restarts/restart_policy.h"

namespace xorcle {

/// @brief Restart on a geometrically growing conflict budget.
///
/// Each restart multiplies the budget by `RestartConfig::geometric_multiplier` and rounds
/// up. Budgets saturate at `SIZE_MAX`, and a multiplier below 1 clamps to 1.
class Geometric final : public RestartPolicy {
public:
    /// @pre `config.geometric_initial_conflicts != 0`.
    explicit Geometric(const RestartConfig& config = {});

    bool enabled() const override;
    bool should_restart_after_conflict() override;
    void note_restart() override;

private:
    double multiplier_;
    std::size_t initial_conflicts_;
    std::size_t conflicts_since_restart_ = 0;
    std::size_t current_budget_ = 0;

    void refresh_budget();
};

} // namespace xorcle
