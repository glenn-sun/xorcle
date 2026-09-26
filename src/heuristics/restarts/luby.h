#pragma once

/// @file luby.h
/// @brief The Luby restart schedule.

#include <cstddef>

#include "heuristics/restarts/restart_policy.h"

namespace xorcle {

/// @brief The Luby sequence value at a 1-indexed position: 1, 1, 2, 1, 1, 2, 4, 1, 1, 2,
/// 1, 1, 2, 4, 8, ..., always a power of two.
///
/// Index 0 returns 1.
std::size_t luby_value(std::size_t index);

/// @brief Restart on `RestartConfig::luby_unit_conflicts` times the Luby sequence.
/// @note The position advances only on a real restart, so a run that never exhausts a
/// budget stays at position 1.
class Luby final : public RestartPolicy {
public:
    /// @pre `config.luby_unit_conflicts != 0`.
    explicit Luby(const RestartConfig& config);

    bool should_restart_after_conflict(const ConflictSummary& conflict) override;
    void note_restart() override;

private:
    std::size_t unit_conflicts_;
    std::size_t conflicts_since_restart_ = 0;
    std::size_t current_budget_ = 0;
    std::size_t luby_index_ = 1;

    void refresh_budget();
};

} // namespace xorcle
