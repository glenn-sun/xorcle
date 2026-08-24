#pragma once

/// @file restart_policy.h
/// @brief When to restart the search: the interface, the configuration, and the
/// "never restart" policy.

#include <cstddef>
#include <memory>

namespace xorcle {

enum class RestartMethod {
    none,
    geometric,
    luby
};

struct RestartConfig {
    RestartMethod method = RestartMethod::none;
    double geometric_multiplier = 1.5;
    std::size_t geometric_initial_conflicts = 100;
    std::size_t luby_unit_conflicts = 100; ///< The budget for a Luby value of 1.
};

/// @brief Stateful restart scheduler driven by conflict counts.
///
/// @warning Restarts are experimental and the schedules here are untuned.
class RestartPolicy {
public:
    virtual ~RestartPolicy() = default;

    virtual bool enabled() const = 0;

    /// @brief Record one non-root conflict; true exactly when it exhausts the active budget.
    ///
    /// Consumes a conflict but does not advance to the next budget.
    virtual bool should_restart_after_conflict() = 0;

    /// @brief Advance to the next budget.
    /// @pre A restart has just happened.
    virtual void note_restart() = 0;
};

class NoRestarts final : public RestartPolicy {
public:
    bool enabled() const override;

    bool should_restart_after_conflict() override;

    void note_restart() override;
};

/// @brief Build the policy named by @p config.
/// @return A policy that is never null; `RestartMethod::none` yields `NoRestarts`.
std::unique_ptr<RestartPolicy> make_restart_policy(const RestartConfig& config);

} // namespace xorcle
