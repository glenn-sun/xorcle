#include "heuristics/restarts/stabilize.h"

#include <cassert>

namespace xorcle {

ModeSchedule::ModeSchedule(std::size_t interval_conflicts)
    : interval_conflicts_(interval_conflicts) {
    assert(interval_conflicts != 0 && "a mode phase must last at least one conflict");
}

std::size_t ModeSchedule::phase_conflicts() const noexcept {
    // Phases 1 and 2 have k = 2, phases 3 and 4 k = 3, and so on.
    const std::size_t k = phase_idx_ == 0 ? 1 : (phase_idx_ + 3) / 2;
    return interval_conflicts_ * k * k;
}

bool ModeSchedule::update_with_conflict() noexcept {
    if (++conflicts_in_phase_ < phase_conflicts()) {
        return false;
    }
    conflicts_in_phase_ = 0;
    ++phase_idx_;
    return true;
}

} // namespace xorcle
