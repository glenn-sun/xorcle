#pragma once

/// @file lbd_restarts.h
/// @brief The restart schedule driven by literal block distance.

#include <cstddef>

#include "heuristics/restarts/restart_policy.h"
#include "util/ema.h"

namespace xorcle {

/// @brief Restart once recent clauses are worse than the run has averaged.
///
/// Glucose's rule over exponential moving averages of the literal block
/// distance: a fast average exceeding the slow one by `RestartConfig::lbd_min_ratio`
/// means learning has degraded. A trail far longer than average cancels that.
/// Both averages run on unbroken: nothing is cleared at a restart or a
/// cancellation.
class LbdRestarts final : public RestartPolicy {
public:
    /// @pre `config.lbd_fast_window`, `config.lbd_slow_window` and
    /// `config.lbd_min_conflicts` are all non-zero.
    explicit LbdRestarts(const RestartConfig& config);

    bool should_restart_after_conflict(const ConflictSummary& conflict) override;
    void note_restart() override;
    void add_to_stats(RunStats& stats) const override;

    std::size_t num_canceled() const noexcept { return canceled_; }
    double fast_lbd() const noexcept { return fast_lbd_.value(); }
    double slow_lbd() const noexcept { return slow_lbd_.value(); }

private:
    Ema fast_lbd_;
    Ema slow_lbd_;
    Ema trail_length_;
    double min_ratio_;
    double trail_ratio_;
    std::size_t min_conflicts_;
    std::size_t conflicts_since_restart_ = 0;
    std::size_t canceled_ = 0; ///< Restarts the trail-length guard canceled once they were due.
};

} // namespace xorcle
