#include "search/RestartPolicy.h"

#include <cmath>
#include <limits>

namespace search {

namespace {

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

RestartPolicy::RestartPolicy(const RestartConfig& config)
    : config_(config) {
    refresh_budget();
}

bool RestartPolicy::enabled() const {
    return config_.method != RestartMethod::none;
}

bool RestartPolicy::should_restart_after_conflict() {
    if (!enabled()) {
        return false;
    }
    ++conflicts_since_restart_;
    return conflicts_since_restart_ >= current_budget_;
}

void RestartPolicy::note_restart() {
    if (!enabled()) {
        return;
    }
    conflicts_since_restart_ = 0;
    if (config_.method == RestartMethod::luby) {
        ++luby_index_;
    }
    refresh_budget();
}

std::size_t RestartPolicy::luby_value(std::size_t index) {
    if (index == 0) {
        return 1;
    }

    std::size_t pow2k = 2;
    while ((pow2k - 1) < index && pow2k <= (std::numeric_limits<std::size_t>::max() / 2)) {
        pow2k *= 2;
    }

    if (index == pow2k - 1) {
        return pow2k / 2;
    }

    std::size_t prev_block = (pow2k / 2) - 1;
    return luby_value(index - prev_block);
}

void RestartPolicy::refresh_budget() {
    switch (config_.method) {
        case RestartMethod::none:
            current_budget_ = 0;
            return;
        case RestartMethod::geometric: {
            if (current_budget_ == 0) {
                current_budget_ = config_.geometric_initial_conflicts == 0 ? 1 : config_.geometric_initial_conflicts;
                return;
            }
            double next_budget = std::ceil(static_cast<double>(current_budget_) * config_.geometric_multiplier);
            if (!std::isfinite(next_budget) ||
                next_budget >= static_cast<double>(std::numeric_limits<std::size_t>::max())) {
                current_budget_ = std::numeric_limits<std::size_t>::max();
            } else {
                std::size_t budget = static_cast<std::size_t>(next_budget);
                current_budget_ = budget == 0 ? 1 : budget;
            }
            return;
        }
        case RestartMethod::luby: {
            std::size_t unit = config_.luby_unit_conflicts == 0 ? 1 : config_.luby_unit_conflicts;
            current_budget_ = clamp_product(unit, luby_value(luby_index_));
            if (current_budget_ == 0) {
                current_budget_ = 1;
            }
            return;
        }
    }
}

} // namespace search
