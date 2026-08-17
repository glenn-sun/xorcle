/// @file test_restart_policy.cpp
/// @brief The restart schedules, against hand-computed sequences.
///
/// The expected Luby values and geometric budgets below were worked out by hand
/// from the definitions, not read off the implementation.

#include <doctest/doctest.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <vector>

#include "heuristics/restarts/geometric.h"
#include "heuristics/restarts/luby.h"
#include "heuristics/restarts/restart_policy.h"

namespace xorcle {

namespace {

/// @brief Feed conflicts to a policy and record the budget each restart took.
///
/// @param policy Policy under test.
/// @param num_restarts How many restarts to observe.
/// @return The conflict count consumed by each restart, in order.
std::vector<std::size_t> observed_budgets(RestartPolicy& policy, std::size_t num_restarts) {
    std::vector<std::size_t> budgets;
    std::size_t conflicts_in_budget = 0;
    // Generous cap: the largest schedule exercised here needs well under this.
    for (std::size_t conflict = 0; budgets.size() < num_restarts && conflict < 1'000'000; ++conflict) {
        ++conflicts_in_budget;
        if (policy.should_restart_after_conflict()) {
            budgets.push_back(conflicts_in_budget);
            conflicts_in_budget = 0;
            policy.note_restart();
        }
    }
    return budgets;
}

} // namespace

TEST_CASE("luby_value: the hand-computed sequence") {
    // 1, 1, 2, 1, 1, 2, 4, 1, 1, 2, 1, 1, 2, 4, 8, ... -- each block is the
    // previous block twice, then the next power of two. Indices are 1-based.
    const std::vector<std::size_t> expected = {
        1, 1, 2, 1, 1, 2, 4, 1, 1, 2, 1, 1, 2, 4, 8,
        1, 1, 2, 1, 1, 2, 4, 1, 1, 2, 1, 1, 2, 4, 8, 16,
    };

    for (std::size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i + 1);
        CHECK(luby_value(i + 1) == expected[i]);
    }

    // Index 0 is outside the 1-based sequence and is defined to return 1.
    CHECK(luby_value(0) == 1);

    // The powers of two sit at positions 2^k - 1.
    CHECK(luby_value(31) == 16);
    CHECK(luby_value(63) == 32);
    CHECK(luby_value(127) == 64);
    CHECK(luby_value(255) == 128);

    // Position 2^k is the start of the next block, so it drops back to 1.
    CHECK(luby_value(32) == 1);
    CHECK(luby_value(64) == 1);
    CHECK(luby_value(128) == 1);
}

TEST_CASE("Luby: budgets are the unit times the sequence") {
    RestartConfig config;
    config.method = RestartMethod::luby;
    config.luby_unit_conflicts = 100;

    Luby policy(config);
    CHECK(policy.enabled());
    CHECK(policy.current_budget() == 100);

    const std::vector<std::size_t> expected = {100, 100, 200, 100, 100, 200, 400, 100, 100, 200};
    CHECK(observed_budgets(policy, expected.size()) == expected);
}

TEST_CASE("Luby: a unit of 1 reproduces the sequence directly") {
    RestartConfig config;
    config.method = RestartMethod::luby;
    config.luby_unit_conflicts = 1;

    Luby policy(config);
    const std::vector<std::size_t> expected = {1, 1, 2, 1, 1, 2, 4, 1, 1, 2, 1, 1, 2, 4, 8};
    CHECK(observed_budgets(policy, expected.size()) == expected);
}

TEST_CASE("Luby: a zero unit is clamped up to one") {
    RestartConfig config;
    config.method = RestartMethod::luby;
    config.luby_unit_conflicts = 0;

    Luby policy(config);
    CHECK(policy.current_budget() == 1);
    const std::vector<std::size_t> expected = {1, 1, 2, 1, 1, 2, 4};
    CHECK(observed_budgets(policy, expected.size()) == expected);
}

TEST_CASE("Luby: an enormous unit saturates rather than wrapping") {
    RestartConfig config;
    config.method = RestartMethod::luby;
    config.luby_unit_conflicts = std::numeric_limits<std::size_t>::max();

    Luby policy(config);
    CHECK(policy.current_budget() == std::numeric_limits<std::size_t>::max());
    policy.note_restart();
    CHECK(policy.current_budget() == std::numeric_limits<std::size_t>::max());
}

TEST_CASE("Geometric: budgets grow by the multiplier, rounding up") {
    RestartConfig config;
    config.method = RestartMethod::geometric;
    config.geometric_initial_conflicts = 100;
    config.geometric_multiplier = 1.5;

    Geometric policy(config);
    CHECK(policy.enabled());
    CHECK(policy.current_budget() == 100);

    // 100, 150, 225, ceil(337.5) = 338, ceil(507) = 507, ceil(760.5) = 761,
    // ceil(1141.5) = 1142, 1713, ceil(2569.5) = 2570, 3855.
    const std::vector<std::size_t> expected = {100, 150, 225, 338, 507, 761, 1142, 1713, 2570, 3855};
    CHECK(observed_budgets(policy, expected.size()) == expected);
}

TEST_CASE("Geometric: a doubling schedule") {
    RestartConfig config;
    config.method = RestartMethod::geometric;
    config.geometric_initial_conflicts = 3;
    config.geometric_multiplier = 2.0;

    Geometric policy(config);
    const std::vector<std::size_t> expected = {3, 6, 12, 24, 48, 96};
    CHECK(observed_budgets(policy, expected.size()) == expected);
}

TEST_CASE("Geometric: a multiplier of 1 gives a constant budget") {
    RestartConfig config;
    config.method = RestartMethod::geometric;
    config.geometric_initial_conflicts = 7;
    config.geometric_multiplier = 1.0;

    Geometric policy(config);
    const std::vector<std::size_t> expected = {7, 7, 7, 7};
    CHECK(observed_budgets(policy, expected.size()) == expected);
}

TEST_CASE("Geometric: degenerate configurations clamp to one") {
    SUBCASE("zero initial budget") {
        RestartConfig config;
        config.method = RestartMethod::geometric;
        config.geometric_initial_conflicts = 0;
        config.geometric_multiplier = 1.5;

        Geometric policy(config);
        CHECK(policy.current_budget() == 1);
        // ceil(1 * 1.5) = 2, then 3, then ceil(4.5) = 5.
        const std::vector<std::size_t> expected = {1, 2, 3, 5};
        CHECK(observed_budgets(policy, expected.size()) == expected);
    }

    SUBCASE("a shrinking multiplier never reaches zero") {
        RestartConfig config;
        config.method = RestartMethod::geometric;
        config.geometric_initial_conflicts = 4;
        config.geometric_multiplier = 0.5;

        Geometric policy(config);
        const std::vector<std::size_t> expected = {4, 2, 1, 1, 1};
        CHECK(observed_budgets(policy, expected.size()) == expected);
    }
}

TEST_CASE("Geometric: growth saturates instead of overflowing") {
    RestartConfig config;
    config.method = RestartMethod::geometric;
    config.geometric_initial_conflicts = std::numeric_limits<std::size_t>::max();
    config.geometric_multiplier = 1.5;

    Geometric policy(config);
    CHECK(policy.current_budget() == std::numeric_limits<std::size_t>::max());
    policy.note_restart();
    CHECK(policy.current_budget() == std::numeric_limits<std::size_t>::max());
    policy.note_restart();
    CHECK(policy.current_budget() == std::numeric_limits<std::size_t>::max());
}

TEST_CASE("NoRestarts: never fires and consumes no budget") {
    NoRestarts policy;
    CHECK_FALSE(policy.enabled());
    for (int i = 0; i < 10'000; ++i) {
        CHECK_FALSE(policy.should_restart_after_conflict());
    }
    policy.note_restart();
    CHECK_FALSE(policy.should_restart_after_conflict());
}

TEST_CASE("make_restart_policy: dispatches on the configured method") {
    SUBCASE("none is the default and yields a disabled policy") {
        const RestartConfig config;
        CHECK(config.method == RestartMethod::none);
        auto policy = make_restart_policy(config);
        REQUIRE(policy != nullptr);
        CHECK_FALSE(policy->enabled());
        CHECK_FALSE(policy->should_restart_after_conflict());
    }

    SUBCASE("geometric") {
        RestartConfig config;
        config.method = RestartMethod::geometric;
        config.geometric_initial_conflicts = 2;
        config.geometric_multiplier = 2.0;
        auto policy = make_restart_policy(config);
        REQUIRE(policy != nullptr);
        CHECK(policy->enabled());
        CHECK(observed_budgets(*policy, 3) == std::vector<std::size_t>{2, 4, 8});
    }

    SUBCASE("luby") {
        RestartConfig config;
        config.method = RestartMethod::luby;
        config.luby_unit_conflicts = 2;
        auto policy = make_restart_policy(config);
        REQUIRE(policy != nullptr);
        CHECK(policy->enabled());
        CHECK(observed_budgets(*policy, 4) == std::vector<std::size_t>{2, 2, 4, 2});
    }
}

TEST_CASE("restart policies: note_restart is what advances the schedule") {
    RestartConfig config;
    config.method = RestartMethod::luby;
    config.luby_unit_conflicts = 10;

    Luby policy(config);
    // Exhausting a budget without calling note_restart leaves the schedule
    // where it was; the solver only advances it after a real restart.
    for (int i = 0; i < 10; ++i) {
        CHECK(policy.should_restart_after_conflict() == (i == 9));
    }
    CHECK(policy.current_budget() == 10);
    CHECK(policy.should_restart_after_conflict());
    policy.note_restart();
    CHECK(policy.current_budget() == 10); // luby_value(2) == 1
    policy.note_restart();
    CHECK(policy.current_budget() == 20); // luby_value(3) == 2
}

} // namespace xorcle
