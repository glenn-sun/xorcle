#include "heuristics/restarts/luby.h"

#include <cassert>
#include <limits>

namespace xorcle {

namespace {

/// @brief Multiply, saturating at `SIZE_MAX` instead of wrapping.
std::size_t clamp_product(std::size_t a, std::size_t b) {
    if (a == 0 || b == 0) {
        return 0;
    }
    constexpr std::size_t max_value = std::numeric_limits<std::size_t>::max();
    if (a > max_value / b) {
        return max_value;
    }
    return a * b;
}

} // namespace

std::size_t
luby_value(std::size_t index) {
    if (index == 0) {
        return 1;
    }

    // Find the smallest complete block 2^k - 1 that reaches `index`.
    std::size_t pow2k = 2;
    while ((pow2k - 1) < index && pow2k <= (std::numeric_limits<std::size_t>::max() / 2)) {
        pow2k *= 2;
    }

    // The last position of a complete block holds that block's power of two.
    if (index == pow2k - 1) {
        return pow2k / 2;
    }

    // Otherwise `index` sits inside one of the two copies of the previous
    // block, so fold it back into the first copy.
    const std::size_t prev_block = (pow2k / 2) - 1;
    return luby_value(index - prev_block);
}

Luby::Luby(const RestartConfig& config)
    : RestartPolicy(config.reuse_trail), unit_conflicts_(config.luby_unit_conflicts) {
    assert(config.luby_unit_conflicts != 0 && "Luby unit budget must be non-zero");
    refresh_budget();
}

bool
Luby::should_restart_after_conflict(const ConflictSummary&) {
    ++conflicts_since_restart_;
    return conflicts_since_restart_ >= current_budget_;
}

void
Luby::note_restart() {
    conflicts_since_restart_ = 0;
    ++luby_index_;
    refresh_budget();
}

void
Luby::refresh_budget() {
    current_budget_ = clamp_product(unit_conflicts_, luby_value(luby_index_));
}

} // namespace xorcle
