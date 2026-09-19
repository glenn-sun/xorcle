#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>

#include "heuristics/propagation/attempt_budget.h"

namespace xorcle {

namespace {

/// @brief A budget on a database large enough, and decayed far enough, that one
/// more learn of either kind lands clear of both clamps.
AttemptBudget settled(double miss_rate, std::size_t window) {
    AttemptBudget budget(miss_rate, window, 1'000'000);
    for (std::size_t learn = 0; learn < 2 * window; ++learn) {
        budget.note_learn(false);
    }
    return budget;
}

/// @brief The factor one learn in time multiplies the budget by.
double hit_factor(double miss_rate, std::size_t window) {
    AttemptBudget budget = settled(miss_rate, window);
    const double before = budget.fraction_of_clauses();
    budget.note_learn(false);
    return budget.fraction_of_clauses() / before;
}

/// @brief The factor one missed learn multiplies the budget by.
double miss_factor(double miss_rate, std::size_t window) {
    AttemptBudget budget = settled(miss_rate, window);
    const double before = budget.fraction_of_clauses();
    budget.note_learn(true);
    return budget.fraction_of_clauses() / before;
}

} // namespace

TEST_CASE("attempt budget: a learn in time decays it over the window") {
    constexpr std::size_t window = 250;
    // The decay is the same whatever the target, so the window alone times it.
    CHECK(hit_factor(0.02, window) == doctest::Approx(std::exp(-1.0 / window)));
    CHECK(hit_factor(0.5, window) == doctest::Approx(std::exp(-1.0 / window)));
    CHECK(hit_factor(1.0, window) == doctest::Approx(std::exp(-1.0 / window)));
    CHECK(hit_factor(0.02, 1000) == doctest::Approx(std::exp(-1.0 / 1000)));
}

TEST_CASE("attempt budget: a missed learn lengthens it by the target's share") {
    constexpr std::size_t window = 250;
    CHECK(miss_factor(0.02, window) == doctest::Approx(std::exp((1.0 - 0.02) / (0.02 * window))));
    CHECK(miss_factor(0.5, window) == doctest::Approx(std::exp((1.0 - 0.5) / (0.5 * window))));
    // A target of one asks for nothing but misses, so a miss moves nothing.
    CHECK(miss_factor(1.0, window) == doctest::Approx(1.0));
}

TEST_CASE("attempt budget: it holds still where misses arrive at the target rate") {
    constexpr std::size_t window = 250;
    for (const double miss_rate : {0.005, 0.05, 0.4}) {
        // One miss in every `1 / miss_rate` learns leaves the budget where it was.
        CHECK(miss_rate * std::log(miss_factor(miss_rate, window)) ==
              doctest::Approx((1.0 - miss_rate) / window));
        CHECK(std::log(hit_factor(miss_rate, window)) == doctest::Approx(-1.0 / window));
    }
}

TEST_CASE("attempt budget: the attempt count is the budget's ceiling") {
    AttemptBudget budget(0.5, 40, 4);
    CHECK(budget.attempts() == 4);
    budget.note_learn(false);
    // 4 * exp(-0.025) is 3.901, so three attempts would cut the stretch short.
    CHECK(budget.attempts() == 4);
    for (int learn = 1; learn < 20; ++learn) {
        budget.note_learn(false);
    }
    // 4 * exp(-0.5) is 2.43.
    CHECK(budget.attempts() == 3);
}

TEST_CASE("attempt budget: it is clamped to between one attempt and a whole sweep") {
    constexpr std::size_t num_clauses = 1000;
    AttemptBudget budget(0.25, 200, num_clauses);
    // A whole sweep to begin with, which is what no budget at all would allow.
    CHECK(budget.attempts() == num_clauses);
    CHECK(budget.fraction_of_clauses() == doctest::Approx(1.0));

    SUBCASE("misses cannot take it past the database") {
        for (int learn = 0; learn < 100; ++learn) {
            budget.note_learn(true);
        }
        CHECK(budget.attempts() == num_clauses);
        CHECK(budget.fraction_of_clauses() == doctest::Approx(1.0));
    }

    SUBCASE("learns in time cannot take it below one attempt") {
        for (int learn = 0; learn < 10'000; ++learn) {
            budget.note_learn(false);
        }
        CHECK(budget.attempts() == 1);
    }
}

TEST_CASE("attempt budget: a resized database re-clamps without rescaling") {
    AttemptBudget budget(0.5, 40, 100);
    for (int learn = 0; learn < 40; ++learn) {
        budget.note_learn(false);
    }
    const std::size_t shrunk_from = budget.attempts();
    REQUIRE(shrunk_from < 100);
    REQUIRE(shrunk_from > 4);

    budget.set_num_clauses(4);
    CHECK(budget.attempts() == 4);

    // Growing the database again leaves the budget where the clamp put it.
    budget.set_num_clauses(100);
    CHECK(budget.attempts() == 4);
}

} // namespace xorcle
