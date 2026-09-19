#pragma once

/// @file attempt_budget.h
/// @brief How long a fruitless propagation stretch may run before it defers.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>

namespace xorcle {

/// @brief An attempt budget steered toward a target miss rate.
///
/// Each learned equation multiplies the budget by `exp(-1 / window)` when it
/// arrived in time and by `exp((1 - miss_rate) / (miss_rate * window))` when it
/// was missed, so the budget holds still exactly where misses arrive at
/// `miss_rate` and falls by a factor of `e` over a `window` of learns with none.
/// It starts at a whole sweep of the database and is clamped to it.
class AttemptBudget {
public:
    /// @pre `0 < miss_rate <= 1`, `window >= 1` and `num_clauses >= 1`.
    AttemptBudget(double miss_rate, std::size_t window, std::size_t num_clauses)
        : miss_factor_(std::exp((1.0 - miss_rate) / (miss_rate * static_cast<double>(window)))),
          hit_factor_(std::exp(-1.0 / static_cast<double>(window))),
          num_clauses_(num_clauses),
          budget_(static_cast<double>(num_clauses)) {
        assert(miss_rate > 0.0 && miss_rate <= 1.0 && "the target miss rate is out of range");
        assert(window >= 1 && "an attempt budget over an empty window");
        assert(num_clauses >= 1 && "an attempt budget over an empty clause database");
    }

    /// @brief Note one learned equation, `missed` when it held below the level
    /// the trail stood at where the propagation found it.
    void note_learn(bool missed) {
        budget_ *= missed ? miss_factor_ : hit_factor_;
        clamp();
    }

    /// @brief Attempts a fruitless stretch gets before its run defers.
    std::size_t attempts() const { return static_cast<std::size_t>(std::ceil(budget_)); }

    /// @brief The budget against the size of the database it is clamped to.
    double fraction_of_clauses() const { return budget_ / static_cast<double>(num_clauses_); }

    /// @brief Follow a database that has grown or shrunk, which re-clamps rather
    /// than rescales.
    /// @pre `num_clauses >= 1`.
    void set_num_clauses(std::size_t num_clauses) {
        assert(num_clauses >= 1 && "an attempt budget over an empty clause database");
        num_clauses_ = num_clauses;
        clamp();
    }

private:
    void clamp() { budget_ = std::clamp(budget_, 1.0, static_cast<double>(num_clauses_)); }

    double miss_factor_;
    double hit_factor_;
    std::size_t num_clauses_;
    double budget_;
};

} // namespace xorcle
