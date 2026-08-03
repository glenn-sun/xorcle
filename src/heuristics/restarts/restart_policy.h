#pragma once

/// @file restart_policy.h
/// @brief When to restart the search: the interface, the configuration, and the
/// degenerate "never restart" policy.

#include <cstddef>
#include <memory>

namespace xorcle {

/// @brief Supported solver restart scheduling methods.
enum class RestartMethod {
    none,      ///< Disable restarts.
    geometric, ///< Geometric conflict-budget growth.
    luby       ///< Luby-sequence conflict budgets.
};

/// @brief Runtime restart configuration passed into the solver.
struct RestartConfig {
    RestartMethod method = RestartMethod::none;    ///< Which schedule to run.
    double geometric_multiplier = 1.5;             ///< Growth factor between geometric budgets.
    std::size_t geometric_initial_conflicts = 100; ///< First geometric budget.
    std::size_t luby_unit_conflicts = 100;         ///< Budget for a Luby value of 1.
};

/// @brief Stateful restart scheduler driven by conflict counts.
///
/// Algorithm 1 says "restart or decide" without saying when; `core/` performs
/// the restart and this decides whether to.
///
/// @warning **Restarts are experimental.** The paper's Conclusion lists them
/// among the parts of Xorcle that are not yet fully implemented, and the CLI
/// marks `--restart=` accordingly. In particular the schedules here were never
/// tuned against the paper's benchmark families, and the default is
/// `RestartMethod::none`.
///
/// The three schedules live in separate files: `NoRestarts` below,
/// `Geometric` in `geometric.h`, and `Luby` in `luby.h`.
class RestartPolicy {
public:
    /// @brief Virtual destructor for interface use.
    virtual ~RestartPolicy() = default;

    /// @brief Return true if restarts are enabled.
    virtual bool enabled() const = 0;

    /// @brief Record one non-root conflict and report whether to restart now.
    /// @return True exactly when the current conflict exhausts the active restart budget.
    /// @note This consumes a conflict from the current budget, but does not advance to
    /// the next budget. Call `note_restart()` only after an actual restart happens.
    virtual bool should_restart_after_conflict() = 0;

    /// @brief Advance to the next restart budget after an actual restart.
    /// @note This must be called only after the solver has really restarted.
    virtual void note_restart() = 0;
};

/// @brief The `RestartMethod::none` policy: never restart.
///
/// A null object rather than a schedule, so it lives with the interface instead
/// of getting a file of its own. It exists so the
/// solver can hold a non-null `RestartPolicy` unconditionally.
class NoRestarts final : public RestartPolicy {
public:
    /// @brief Always false.
    bool enabled() const override;

    /// @brief Always false; no budget is ever consumed.
    bool should_restart_after_conflict() override;

    /// @brief No-op.
    void note_restart() override;
};

/// @brief Build the policy named by @p config.
/// @param config Resolved restart configuration.
/// @return A policy that is never null; `RestartMethod::none` yields `NoRestarts`.
std::unique_ptr<RestartPolicy> make_restart_policy(const RestartConfig& config);

} // namespace xorcle
