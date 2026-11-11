/// @file test_restart_policy.cpp
/// @brief The restart schedules, against hand-computed sequences.
///
/// The expected Luby values below were worked out by hand from the definition,
/// not read off the implementation.

#include <doctest/doctest.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "heuristics/restarts/lbd_restarts.h"
#include "heuristics/restarts/luby.h"
#include "heuristics/restarts/restart_policy.h"
#include "heuristics/restarts/stabilize.h"
#include "heuristics_test_support.h"

namespace xorcle {

namespace {

/// @brief A conflict the fixed schedules ignore.
constexpr ConflictSummary any_conflict{.lbd = 1, .trail_length = 1};

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
        if (policy.should_restart_after_conflict(any_conflict)) {
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
    RestartConfig config = default_restart_config();
    config.method = RestartMethod::luby;
    config.luby_unit_conflicts = 100;

    Luby policy(config);

    const std::vector<std::size_t> expected = {100, 100, 200, 100, 100, 200, 400, 100, 100, 200};
    CHECK(observed_budgets(policy, expected.size()) == expected);
}

TEST_CASE("Luby: a unit of 1 reproduces the sequence directly") {
    RestartConfig config = default_restart_config();
    config.method = RestartMethod::luby;
    config.luby_unit_conflicts = 1;

    Luby policy(config);
    const std::vector<std::size_t> expected = {1, 1, 2, 1, 1, 2, 4, 1, 1, 2, 1, 1, 2, 4, 8};
    CHECK(observed_budgets(policy, expected.size()) == expected);
}

TEST_CASE("Luby: an enormous unit saturates rather than wrapping") {
    RestartConfig config = default_restart_config();
    config.method = RestartMethod::luby;
    config.luby_unit_conflicts = std::numeric_limits<std::size_t>::max();

    // A wrapped product would be a small budget that fires almost at once; a
    // saturated one never fires here, before or after the schedule advances.
    Luby policy(config);
    CHECK_FALSE(policy.should_restart_after_conflict(any_conflict));
    policy.note_restart();
    CHECK_FALSE(policy.should_restart_after_conflict(any_conflict));
}

TEST_CASE("make_restart_policy: dispatches on the configured method") {
    SUBCASE("none is the default and yields a disabled policy") {
        const RestartConfig config = default_restart_config();
        CHECK(config.method == RestartMethod::none);
        auto policy = make_restart_policy(config);
        REQUIRE(policy != nullptr);
        CHECK_FALSE(policy->should_restart_after_conflict(any_conflict));
    }

    SUBCASE("luby") {
        RestartConfig config = default_restart_config();
        config.method = RestartMethod::luby;
        config.luby_unit_conflicts = 2;
        auto policy = make_restart_policy(config);
        REQUIRE(policy != nullptr);
        CHECK(observed_budgets(*policy, 4) == std::vector<std::size_t>{2, 2, 4, 2});
    }

    SUBCASE("lbd") {
        RestartConfig config = default_restart_config();
        config.method = RestartMethod::lbd;
        auto policy = make_restart_policy(config);
        REQUIRE(policy != nullptr);
        CHECK_FALSE(policy->should_restart_after_conflict({.lbd = 4, .trail_length = 20}));
    }
}

TEST_CASE("LbdRestarts: a steady stream of clauses never restarts") {
    RestartConfig config = default_restart_config();
    config.method = RestartMethod::lbd;

    // Both averages read back the same constant exactly, so no margin above 1
    // can ever be cleared.
    LbdRestarts policy(config);
    for (std::size_t conflict = 0; conflict < 10'000; ++conflict) {
        CHECK_FALSE(policy.should_restart_after_conflict({.lbd = 4, .trail_length = 20}));
    }
}

TEST_CASE("LbdRestarts: clauses turning worse than the run's average restart") {
    RestartConfig config = default_restart_config();
    config.method = RestartMethod::lbd;

    LbdRestarts policy(config);
    for (std::size_t conflict = 0; conflict < 1'000; ++conflict) {
        REQUIRE_FALSE(policy.should_restart_after_conflict({.lbd = 2, .trail_length = 20}));
    }

    std::size_t conflicts_until_restart = 0;
    while (!policy.should_restart_after_conflict({.lbd = 20, .trail_length = 20})) {
        ++conflicts_until_restart;
        REQUIRE(conflicts_until_restart < 1'000);
    }
    // Within the fast window, since that is how long the fast average takes to
    // notice.
    CHECK(conflicts_until_restart < config.lbd_fast_window);
}

TEST_CASE("LbdRestarts: a trail far above average blocks the restart") {
    RestartConfig config = default_restart_config();
    config.method = RestartMethod::lbd;

    LbdRestarts policy(config);
    for (std::size_t conflict = 0; conflict < 1'000; ++conflict) {
        REQUIRE_FALSE(policy.should_restart_after_conflict({.lbd = 2, .trail_length = 20}));
    }
    // Not for long: the trail average has the slow window, so it catches up
    // with the longer trail within about a thousand conflicts.
    for (std::size_t conflict = 0; conflict < 100; ++conflict) {
        CHECK_FALSE(policy.should_restart_after_conflict({.lbd = 20, .trail_length = 400}));
    }

    // The block is on the trail, not on the clauses: the same stream restarts
    // once the trail comes back down.
    std::size_t conflicts_until_restart = 0;
    while (!policy.should_restart_after_conflict({.lbd = 20, .trail_length = 20})) {
        ++conflicts_until_restart;
        REQUIRE(conflicts_until_restart < 1'000);
    }
}

TEST_CASE("LbdRestarts: a zero trail ratio never blocks") {
    RestartConfig config = default_restart_config();
    config.method = RestartMethod::lbd;
    config.lbd_trail_ratio = 0.0;

    LbdRestarts policy(config);
    for (std::size_t conflict = 0; conflict < 1'000; ++conflict) {
        REQUIRE_FALSE(policy.should_restart_after_conflict({.lbd = 2, .trail_length = 20}));
    }
    std::size_t conflicts_until_restart = 0;
    while (!policy.should_restart_after_conflict({.lbd = 20, .trail_length = 400})) {
        ++conflicts_until_restart;
        REQUIRE(conflicts_until_restart < 1'000);
    }
}

TEST_CASE("LbdRestarts: the minimum interval is waited out after a restart") {
    RestartConfig config = default_restart_config();
    config.method = RestartMethod::lbd;
    config.lbd_min_conflicts = 500;

    LbdRestarts policy(config);
    for (std::size_t conflict = 0; conflict < 1'000; ++conflict) {
        REQUIRE_FALSE(policy.should_restart_after_conflict({.lbd = 2, .trail_length = 20}));
    }
    std::size_t guard = 0;
    while (!policy.should_restart_after_conflict({.lbd = 20, .trail_length = 20})) {
        REQUIRE(++guard < 1'000);
    }
    policy.note_restart();

    // The averages call for another restart at once, the fast one carrying its
    // samples across the restart; only the interval holds it back.
    for (std::size_t conflict = 1; conflict < 500; ++conflict) {
        CHECK_FALSE(policy.should_restart_after_conflict({.lbd = 20, .trail_length = 20}));
    }
    CHECK(policy.should_restart_after_conflict({.lbd = 20, .trail_length = 20}));
}

TEST_CASE("restart policies: note_restart is what advances the schedule") {
    RestartConfig config = default_restart_config();
    config.method = RestartMethod::luby;
    config.luby_unit_conflicts = 10;

    Luby policy(config);
    // Exhausting a budget without calling note_restart leaves the schedule
    // where it was; the solver only advances it after a real restart, so the
    // exhausted budget keeps firing until it does.
    for (int i = 0; i < 10; ++i) {
        CHECK(policy.should_restart_after_conflict(any_conflict) == (i == 9));
    }
    CHECK(policy.should_restart_after_conflict(any_conflict));

    // Advancing gives luby_value(2) == 1 then luby_value(3) == 2, i.e. 10 and 20.
    policy.note_restart();
    CHECK(observed_budgets(policy, 2) == std::vector<std::size_t>{10, 20});
}

TEST_CASE("ModeSchedule: focused 1, then stable and focused k squared for k = 2, 3") {
    // Interval 2: focused 2, stable 8, focused 8, stable 18, focused 18.
    ModeSchedule schedule(2);
    const std::pair<SearchMode, std::size_t> phases[]{
        {SearchMode::focused, 2}, {SearchMode::stable, 8}, {SearchMode::focused, 8},
        {SearchMode::stable, 18}, {SearchMode::focused, 18}};
    std::size_t switches = 0;
    for (const auto& [mode, conflicts] : phases) {
        for (std::size_t conflict = 0; conflict < conflicts; ++conflict) {
            CHECK(schedule.mode() == mode);
            const bool ended = schedule.update_with_conflict();
            CHECK(ended == (conflict + 1 == conflicts));
            switches += ended ? 1U : 0U;
        }
    }
    CHECK(switches == 5);
    CHECK(schedule.mode() == SearchMode::stable);
}

} // namespace xorcle
