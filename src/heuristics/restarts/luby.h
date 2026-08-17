#pragma once

/// @file luby.h
/// @brief The Luby restart schedule.

#include <cstddef>

#include "heuristics/restarts/restart_policy.h"

namespace xorcle {

/// @brief The Luby sequence value at a 1-indexed position, a power of two.
///
/// The sequence is 1, 1, 2, 1, 1, 2, 4, 1, 1, 2, 1, 1, 2, 4, 8, ... — each block
/// is the previous block twice, then the next power of two. Index 0 returns 1.
std::size_t luby_value(std::size_t index);

/// @brief Restart on `luby_unit_conflicts` times the Luby sequence.
///
/// With the default unit of 100 the budgets run 100, 100, 200, 100, 100, 200,
/// 400, ... The position advances only on a real restart, so a run that never
/// exhausts a budget stays at position 1.
///
/// @note Experimental; see `RestartPolicy`.
class Luby final : public RestartPolicy {
public:
    /// @param config Restart configuration; only `luby_unit_conflicts` is read.
    explicit Luby(const RestartConfig& config = {});

    bool enabled() const override;
    bool should_restart_after_conflict() override;
    void note_restart() override;

    std::size_t current_budget() const { return current_budget_; }

private:
    std::size_t unit_conflicts_;              ///< Budget for a Luby value of 1.
    std::size_t conflicts_since_restart_ = 0; ///< Conflicts counted against the active budget.
    std::size_t current_budget_ = 0;          ///< The active budget.
    std::size_t luby_index_ = 1;              ///< 1-indexed position in the Luby sequence.

    void refresh_budget();
};

} // namespace xorcle
