#include "heuristics/restarts/lbd.h"

#include <cassert>
#include <cstddef>

namespace xorcle {

Lbd::Lbd(const RestartConfig& config)
    : RestartPolicy(config.reuse_trail),
      fast_lbd_(config.lbd_fast_window), slow_lbd_(config.lbd_slow_window),
      trail_size_(config.lbd_slow_window),
      margin_(config.lbd_margin),
      block_factor_(config.lbd_block_factor), min_conflicts_(config.lbd_min_conflicts) {
    assert(config.lbd_min_conflicts != 0 && "LBD restart interval must be non-zero");
}

bool
Lbd::enabled() const {
    return true;
}

bool
Lbd::should_restart_after_conflict(const ConflictSummary& conflict) {
    ++conflicts_since_restart_;

    const double lbd = static_cast<double>(conflict.lbd);
    fast_lbd_.update(lbd);
    slow_lbd_.update(lbd);

    const double trail_size = static_cast<double>(conflict.trail_size);
    trail_size_.update(trail_size);
    if (block_factor_ > 0.0 && trail_size > block_factor_ * trail_size_.value()) {
        ++blocked_;
        fast_lbd_.reset();
        return false;
    }

    if (conflicts_since_restart_ < min_conflicts_) {
        return false;
    }
    return fast_lbd_.value() > margin_ * slow_lbd_.value();
}

RestartStats
Lbd::stats() const {
    return RestartStats{.blocked = blocked_,
                        .fast_lbd = fast_lbd_.value(),
                        .slow_lbd = slow_lbd_.value()};
}

void
Lbd::note_restart() {
    conflicts_since_restart_ = 0;
    // The clauses either side of a restart are not comparable, so the fast
    // average is earned again rather than carried over.
    fast_lbd_.reset();
}

} // namespace xorcle
