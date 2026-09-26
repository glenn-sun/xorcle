#include "heuristics/restarts/lbd_restarts.h"

#include <cassert>

namespace xorcle {

LbdRestarts::LbdRestarts(const RestartConfig& config)
    : RestartPolicy(config.reuse_trail),
      fast_lbd_(config.lbd_fast_window), slow_lbd_(config.lbd_slow_window),
      trail_length_(config.lbd_slow_window),
      min_ratio_(config.lbd_min_ratio),
      trail_ratio_(config.lbd_trail_ratio), min_conflicts_(config.lbd_min_conflicts) {
    assert(config.lbd_min_conflicts != 0 && "LBD restart interval must be non-zero");
}

bool
LbdRestarts::should_restart_after_conflict(const ConflictSummary& conflict) {
    ++conflicts_since_restart_;

    const double lbd = static_cast<double>(conflict.lbd);
    fast_lbd_.update(lbd);
    slow_lbd_.update(lbd);

    const double trail_length = static_cast<double>(conflict.trail_length);
    trail_length_.update(trail_length);

    if (conflicts_since_restart_ < min_conflicts_) {
        return false;
    }
    if (fast_lbd_.value() <= min_ratio_ * slow_lbd_.value()) {
        return false;
    }
    // Last, so that a cancellation counts only a restart that was otherwise due.
    if (trail_ratio_ > 0.0 && trail_length > trail_ratio_ * trail_length_.value()) {
        ++canceled_;
        return false;
    }
    return true;
}

void
LbdRestarts::add_to_stats(RunStats& stats) const {
    stats.num_canceled_restarts = canceled_;
    stats.fast_lbd = fast_lbd_.value();
    stats.slow_lbd = slow_lbd_.value();
}

void
LbdRestarts::note_restart() {
    conflicts_since_restart_ = 0;
}

} // namespace xorcle
