#pragma once

/// @file restart_policy.h
/// @brief When to restart the search: the interface, the configuration, and the
/// "never restart" policy.

#include <cstddef>
#include <memory>

#include "util/stats.h"

namespace xorcle {

enum class RestartMethod {
    none,
    luby,
    lbd
};

struct RestartConfig {
    RestartMethod method = RestartMethod::none;
    /// @brief The budget for a Luby value of 1; `Luby` requires it nonzero.
    std::size_t luby_unit_conflicts = 1;

    std::size_t lbd_fast_window = 33;
    std::size_t lbd_slow_window = 1000;
    /// @brief How far the fast average must exceed the slow one.
    double lbd_min_ratio = 1.1;
    std::size_t lbd_min_conflicts = 1000; ///< Conflicts an LBD restart waits out.
    /// @brief Trail length, relative to its average, that blocks a restart; `0` never blocks.
    double lbd_trail_ratio = 1.4;

    bool operator==(const RestartConfig&) const = default;
};

/// @brief What a conflict tells a restart policy.
struct ConflictSummary {
    std::size_t lbd;        ///< Of the asserting clause just derived.
    std::size_t trail_length; ///< Trail rows at the conflict, before the backjump.
};

/// @brief Stateful restart scheduler driven by the conflicts as they happen.
class RestartPolicy {
public:
    virtual ~RestartPolicy() = default;

    /// @brief Record one non-root conflict; true exactly when it calls for a restart.
    ///
    /// Consumes the conflict but does not advance to the next budget. The fixed
    /// schedules ignore `conflict`.
    virtual bool should_restart_after_conflict(const ConflictSummary& conflict) = 0;

    /// @brief Advance to the next budget.
    /// @pre A restart has just happened.
    virtual void note_restart() = 0;

    /// @brief Add this policy's own counters to `stats`; the default has none.
    virtual void add_to_stats(RunStats& stats) const;
};

class NoRestarts final : public RestartPolicy {
public:
    bool should_restart_after_conflict(const ConflictSummary& conflict) override;

    void note_restart() override;
};

/// @brief Build the policy named by @p config.
/// @return A policy that is never null; `RestartMethod::none` yields `NoRestarts`.
std::unique_ptr<RestartPolicy> make_restart_policy(const RestartConfig& config);

} // namespace xorcle
