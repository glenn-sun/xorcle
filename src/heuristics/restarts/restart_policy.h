#pragma once

/// @file restart_policy.h
/// @brief When to restart the search: the interface, the configuration, and the
/// "never restart" policy.

#include <cstddef>
#include <memory>

namespace xorcle {

enum class RestartMethod {
    none,      ///< Disable restarts.
    geometric, ///< Geometric conflict-budget growth.
    luby       ///< Luby-sequence conflict budgets.
};

struct RestartConfig {
    RestartMethod method = RestartMethod::none;    ///< Which schedule to run.
    double geometric_multiplier = 1.5;             ///< Growth factor between geometric budgets.
    std::size_t geometric_initial_conflicts = 100; ///< First geometric budget.
    std::size_t luby_unit_conflicts = 100;         ///< Budget for a Luby value of 1.
};

/// @brief Stateful restart scheduler driven by conflict counts. `core/` performs
/// the restart; this decides whether to.
///
/// @warning **Restarts are experimental.** The schedules here are untuned, the
/// CLI marks `--restart=` accordingly, and the default is `RestartMethod::none`.
class RestartPolicy {
public:
    virtual ~RestartPolicy() = default;

    virtual bool enabled() const = 0;

    /// @brief Record one non-root conflict; true exactly when it exhausts the
    /// active restart budget. Consumes a conflict but does not advance to the
    /// next budget; only `note_restart()` does that.
    virtual bool should_restart_after_conflict() = 0;

    /// @brief Advance to the next budget.
    /// @pre A restart has just happened. Nothing inside a policy can see the
    /// solver, so this one is on the caller and is not asserted; calling it
    /// without restarting silently skips a budget.
    virtual void note_restart() = 0;
};

/// @brief The `RestartMethod::none` policy: never restart. A null object, so the
/// solver can hold a non-null `RestartPolicy` unconditionally.
class NoRestarts final : public RestartPolicy {
public:
    bool enabled() const override;

    /// @brief Always false; no budget is ever consumed.
    bool should_restart_after_conflict() override;

    void note_restart() override;
};

/// @brief Build the policy named by @p config.
/// @return A policy that is never null; `RestartMethod::none` yields `NoRestarts`.
std::unique_ptr<RestartPolicy> make_restart_policy(const RestartConfig& config);

} // namespace xorcle
