#pragma once

/// @file restart_policy.h
/// @brief When to restart the search: the interface, the configuration, and the
/// "never restart" policy.

#include <cstddef>
#include <memory>

namespace xorcle {

enum class RestartMethod {
    none,
    luby,
    lbd
};

struct RestartConfig {
    RestartMethod method = RestartMethod::none;
    /// @brief The budget for a Luby value of 1. No default: the schedule requires
    /// a value.
    std::size_t luby_unit_conflicts = 0;

    std::size_t lbd_fast_window = 33;
    std::size_t lbd_slow_window = 1000;
    /// @brief How far the fast average must exceed the slow one.
    double lbd_min_ratio = 1.1;
    std::size_t lbd_min_conflicts = 1000; ///< Conflicts an LBD restart waits out.
    /// @brief Trail length, relative to its average, that blocks a restart; `0` never blocks.
    double lbd_trail_ratio = 1.4;

    /// @brief Whether a restart keeps the decisions it would immediately re-make.
    bool reuse_trail = false;

};

/// @brief What a conflict tells a restart policy.
struct ConflictSummary {
    std::size_t lbd;        ///< Of the asserting clause just derived.
    std::size_t trail_length; ///< Trail rows at the conflict, before the backjump.
};

/// @brief What a policy can report about its own decisions.
///
/// Only `LbdRestarts` fills any of it; the fixed schedules have nothing to say, so the
/// base returns zeroes and the statistics report the section only when a
/// schedule is in force.
struct RestartStats {
    /// @brief Restarts the trail-length guard canceled once they were due.
    std::size_t canceled = 0;
    double fast_lbd = 0.0; ///< The fast average as the run ended.
    double slow_lbd = 0.0; ///< The slow average as the run ended.
};

/// @brief Stateful restart scheduler driven by the conflicts as they happen.
class RestartPolicy {
public:
    explicit RestartPolicy(bool reuse_trail = false) : reuse_trail_(reuse_trail) {}

    virtual ~RestartPolicy() = default;

    /// @brief Whether a restart backjumps to the deepest level the decision
    /// procedure would rebuild anyway, rather than to the root.
    bool reuses_trail() const noexcept { return reuse_trail_; }

    /// @brief Record one non-root conflict; true exactly when it calls for a restart.
    ///
    /// Consumes the conflict but does not advance to the next budget. The fixed
    /// schedules ignore `conflict`.
    virtual bool should_restart_after_conflict(const ConflictSummary& conflict) = 0;

    /// @brief Advance to the next budget.
    /// @pre A restart has just happened.
    virtual void note_restart() = 0;

    /// @brief What the policy has to report; zeroes unless it overrides this.
    virtual RestartStats stats() const { return {}; }

private:
    bool reuse_trail_;
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
