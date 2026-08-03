#pragma once

/// @file luby.h
/// @brief The Luby restart schedule.

#include <cstddef>

#include "heuristics/restarts/restart_policy.h"

namespace xorcle {

/// @brief Return the Luby sequence value at a 1-indexed position.
///
/// The sequence is 1, 1, 2, 1, 1, 2, 4, 1, 1, 2, 1, 1, 2, 4, 8, ... — each
/// block is the previous block twice over, followed by the next power of two.
///
/// @param index 1-indexed position. Index 0 also returns 1.
/// @return The value at that position, a power of two.
std::size_t luby_value(std::size_t index);

/// @brief Restart on `luby_unit_conflicts` times the Luby sequence.
///
/// With the default unit of 100 the budgets run
/// 100, 100, 200, 100, 100, 200, 400, 100, ...
///
/// The sequence position advances only on an actual restart, so a run that
/// never exhausts a budget stays at position 1.
///
/// @note Experimental; see `RestartPolicy`.
class Luby final : public RestartPolicy {
public:
    /// @brief Construct from a resolved restart configuration.
    /// @param config Restart configuration; only `luby_unit_conflicts` is read.
    explicit Luby(const RestartConfig& config = {});

    /// @brief Always true.
    bool enabled() const override;

    /// @copydoc RestartPolicy::should_restart_after_conflict
    bool should_restart_after_conflict() override;

    /// @copydoc RestartPolicy::note_restart
    void note_restart() override;

    /// @brief The conflict budget currently being counted down.
    std::size_t current_budget() const { return current_budget_; }

private:
    std::size_t unit_conflicts_;              ///< Budget for a Luby value of 1.
    std::size_t conflicts_since_restart_ = 0; ///< Conflicts counted against the active budget.
    std::size_t current_budget_ = 0;          ///< The active budget.
    std::size_t luby_index_ = 1;              ///< 1-indexed position in the Luby sequence.

    /// @brief Recompute the active conflict budget from current state.
    void refresh_budget();
};

} // namespace xorcle
