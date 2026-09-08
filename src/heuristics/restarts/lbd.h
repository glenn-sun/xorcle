#pragma once

/// @file lbd.h
/// @brief The restart schedule driven by literal block distance.

#include <cstddef>

#include "heuristics/restarts/restart_policy.h"
#include "util/ema.h"

namespace xorcle {

/// @brief Restart once recent clauses are worse than the run has averaged.
///
/// Glucose's rule over exponential moving averages of the literal block
/// distance: a fast average exceeding the slow one by `RestartConfig::lbd_margin`
/// means learning has degraded. A trail far longer than average blocks that and
/// clears the fast average instead, on the grounds that a long trail is close to
/// a model.
class Lbd final : public RestartPolicy {
public:
    /// @pre `config.lbd_fast_window`, `config.lbd_slow_window` and
    /// `config.lbd_min_conflicts` are all non-zero.
    explicit Lbd(const RestartConfig& config = {});

    bool enabled() const override;
    bool should_restart_after_conflict(const ConflictSummary& conflict) override;
    void note_restart() override;
    RestartStats stats() const override;

private:
    Ema fast_lbd_;
    Ema slow_lbd_;
    Ema trail_size_;
    double margin_;
    double block_factor_;
    std::size_t min_conflicts_;
    std::size_t conflicts_since_restart_ = 0;
    std::size_t blocked_ = 0;
};

} // namespace xorcle
