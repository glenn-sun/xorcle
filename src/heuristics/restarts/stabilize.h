#pragma once

/// @file stabilize.h
/// @brief Alternating the search between a focused mode and a stable mode.

#include <cstddef>

#include "heuristics/restarts/restart_policy.h"

namespace xorcle {

/// @brief Focused mode restarts and decays fast; stable mode is the search without restarts.
enum class SearchMode { focused, stable };

struct StabilizeConfig {
    std::size_t interval_conflicts; ///< The length of the first focused phase.
    /// @brief The restarts of focused mode, whose averages see only focused-mode conflicts.
    RestartConfig focused_restarts;
};

/// @brief The mode of each search conflict: focused for one interval, then stable and
/// focused for `k * k` intervals each, for `k = 2, 3, ...`.
class ModeSchedule {
public:
    /// @pre `interval_conflicts > 0`.
    explicit ModeSchedule(std::size_t interval_conflicts);

    /// @brief Count a search conflict, and return whether it ended the mode's phase.
    bool update_with_conflict() noexcept;

    SearchMode mode() const noexcept {
        return phase_idx_ % 2 == 0 ? SearchMode::focused : SearchMode::stable;
    }

private:
    std::size_t phase_conflicts() const noexcept;

    std::size_t interval_conflicts_;
    std::size_t phase_idx_ = 0;
    std::size_t conflicts_in_phase_ = 0;
};

} // namespace xorcle
