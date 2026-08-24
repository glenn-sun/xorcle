#include "heuristics/restarts/geometric.h"

#include <cassert>
#include <cmath>
#include <limits>

namespace xorcle {

Geometric::Geometric(const RestartConfig& config)
    : multiplier_(config.geometric_multiplier),
      initial_conflicts_(config.geometric_initial_conflicts) {
    assert(config.geometric_initial_conflicts != 0 &&
           "geometric initial budget must be non-zero");
    refresh_budget();
}

bool
Geometric::enabled() const {
    return true;
}

bool
Geometric::should_restart_after_conflict() {
    ++conflicts_since_restart_;
    return conflicts_since_restart_ >= current_budget_;
}

void
Geometric::note_restart() {
    conflicts_since_restart_ = 0;
    refresh_budget();
}

void
Geometric::refresh_budget() {
    // A zero budget only occurs before the first refresh, so the constructor's
    // call establishes the initial budget and every later one grows it.
    if (current_budget_ == 0) {
        current_budget_ = initial_conflicts_;
        return;
    }

    const double next_budget = std::ceil(static_cast<double>(current_budget_) * multiplier_);
    if (!std::isfinite(next_budget) ||
        next_budget >= static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        current_budget_ = std::numeric_limits<std::size_t>::max();
    } else if (next_budget < 1.0) {
        // Clamped up to 1. Testing the double rather than the converted value
        // also keeps a negative multiplier out of an undefined conversion.
        current_budget_ = 1;
    } else {
        current_budget_ = static_cast<std::size_t>(next_budget);
    }
}

} // namespace xorcle
