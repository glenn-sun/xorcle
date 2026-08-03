#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "core/watch.h"
#include "heuristics/selection/clause_selector.h"
#include "heuristics/selection/cyclic.h"
#include "heuristics/selection/leading_term_watchlist.h"
#include "heuristics_test_support.h"

namespace xorcle {

namespace {

/// @brief Drain a selector, collecting the ids it hands out.
std::vector<ClauseId> drain(ClauseSelector& selector) {
    std::vector<ClauseId> out;
    while (const std::optional<ClauseId> id = selector.next()) {
        out.push_back(*id);
    }
    return out;
}

} // namespace

TEST_CASE_TEMPLATE("cyclic: round-robin order and quiescence", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    Cyclic selector(4);
    const Trail trail(backend, 4);

    selector.add_starting_clause(make_clause(backend, 4, {{0}}));
    selector.add_starting_clause(make_clause(backend, 4, {{1}}));
    selector.add_starting_clause(make_clause(backend, 4, {{2}}));
    REQUIRE(selector.num_clauses() == 3);

    SUBCASE("a fresh selector starts at the most recently added clause") {
        // One full cycle, then quiescence.
        CHECK(drain(selector) == std::vector<ClauseId>{2, 0, 1});
    }

    SUBCASE("learning an equation refreshes the budget without moving the cursor") {
        CHECK(selector.next() == std::optional<ClauseId>{2});
        CHECK(selector.next() == std::optional<ClauseId>{0});
        selector.update_with_propagation(make_equation_with(backend, 4, {3}), trail);
        // The budget restarts, so the cycle continues from clause 1 for a full
        // pass rather than stopping after one more.
        CHECK(drain(selector) == std::vector<ClauseId>{1, 2, 0});
    }

    SUBCASE("inactive clauses are skipped and still count against the budget") {
        selector.set_clause_active(0, false);
        CHECK(drain(selector) == std::vector<ClauseId>{2, 1});
    }

    SUBCASE("a decision clause hint moves the cursor once") {
        selector.update_with_decision_clause(ClauseId{1});
        CHECK(selector.next() == std::optional<ClauseId>{1});
        CHECK(selector.next() == std::optional<ClauseId>{2});
    }

    SUBCASE("an inactive or out-of-range hint is ignored") {
        selector.set_clause_active(1, false);
        selector.update_with_decision_clause(ClauseId{1});
        CHECK(selector.next() == std::optional<ClauseId>{2});

        selector.update_with_decision_clause(ClauseId{99});
        CHECK(selector.next() == std::optional<ClauseId>{0});
    }

    SUBCASE("compaction moves the cursor onto an active clause") {
        const CompactionTransform transform;
        selector.set_clause_active(2, false);
        selector.set_clause_active(0, false);
        selector.update_with_compaction(transform, trail);
        CHECK(selector.next() == std::optional<ClauseId>{1});
    }

    SUBCASE("an empty selector is immediately quiescent") {
        Cyclic empty(4);
        CHECK(empty.next() == std::nullopt);
    }
}

TEST_CASE_TEMPLATE("leading-term watchlist: bucketing and queueing", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    LeadingTermWatchlist selector(4);
    Trail trail(backend, 4);

    SUBCASE("a unit clause is queued immediately, a non-unit one is not") {
        selector.add_starting_clause(make_clause(backend, 4, {{0}, {1}}));
        CHECK(selector.next() == std::nullopt);

        selector.add_starting_clause(make_clause(backend, 4, {{2}}));
        CHECK(selector.next() == std::optional<ClauseId>{1});
        CHECK(selector.next() == std::nullopt);
    }

    SUBCASE("only the buckets a learned equation can reach are woken") {
        selector.add_starting_clause(make_clause(backend, 4, {{0}, {3}})); // buckets 0 and 3
        selector.add_starting_clause(make_clause(backend, 4, {{1}, {3}})); // buckets 1 and 3
        selector.add_starting_clause(make_clause(backend, 4, {{2}, {3}})); // buckets 2 and 3

        // Learn x0 = 1 so column 0 becomes a pivot, then learn an equation
        // leading on column 2. Bucket 2 is the exact match; bucket 0 is a pivot
        // below it, so reduction could lift such a row up to column 2. Bucket 1
        // is below but is not a pivot, so clause 1 stays asleep.
        trail.learn(make_equation_with(backend, 4, {0, 4}), ClauseId{0});
        selector.update_with_propagation(make_equation_with(backend, 4, {2}), trail);

        const std::vector<ClauseId> woken = drain(selector);
        CHECK(woken.size() == 2);
        CHECK(std::find(woken.begin(), woken.end(), ClauseId{1}) == woken.end());
    }

    SUBCASE("the work list is a deque: exact matches on top, lower pivots underneath") {
        selector.add_starting_clause(make_clause(backend, 4, {{0}, {3}})); // id 0, buckets 0 and 3
        selector.add_starting_clause(make_clause(backend, 4, {{2}, {3}})); // id 1, buckets 2 and 3
        selector.add_starting_clause(make_clause(backend, 4, {{4}}));      // id 2, a unit clause

        // The unit clause is already queued. Learning an equation leading on
        // column 2 pushes bucket 2 to the top and bucket 0 -- a pivot below --
        // to the bottom, so the queued unit clause ends up between them.
        trail.learn(make_equation_with(backend, 4, {0, 4}), ClauseId{0});
        selector.update_with_propagation(make_equation_with(backend, 4, {2}), trail);

        // A stack, which is what the 0.1 tree had committed, would push both
        // buckets to the same end and yield 1, 0, 2 instead.
        CHECK(drain(selector) == std::vector<ClauseId>{1, 2, 0});
    }

    SUBCASE("an asserting clause is queued above every unit clause") {
        selector.add_starting_clause(make_clause(backend, 4, {{0}}));      // id 0, unit
        selector.add_starting_clause(make_clause(backend, 4, {{1}}));      // id 1, unit
        selector.add_starting_clause(make_clause(backend, 4, {{2}, {3}})); // id 2, non-unit

        selector.add_asserting_clause(make_clause(backend, 4, {{3}, {4}}), trail, 2);
        CHECK(drain(selector) == std::vector<ClauseId>{3, 1, 0});
    }

    SUBCASE("inactive clauses are dropped as they are popped") {
        selector.add_starting_clause(make_clause(backend, 4, {{0}}));
        selector.add_starting_clause(make_clause(backend, 4, {{1}}));
        selector.set_clause_active(0, false);
        CHECK(drain(selector) == std::vector<ClauseId>{1});
    }

    SUBCASE("moving a watch moves the clause between buckets") {
        selector.add_starting_clause(make_clause(backend, 4, {{0}, {1}, {2}}));
        Clause& clause = selector.clause(0);
        REQUIRE(clause.rows() == 3);

        // The engine writes the new watch first, then reports it.
        const EquationId old_watch = clause.watched1;
        clause.watched1 = 2;
        selector.update_with_new_watch(0, old_watch, WatchType::one, trail);

        // Bucket 1 no longer holds the clause, so an equation leading there
        // wakes nothing; bucket 2 does.
        selector.update_with_propagation(make_equation_with(backend, 4, {1}), trail);
        CHECK(selector.next() == std::nullopt);

        selector.update_with_propagation(make_equation_with(backend, 4, {2}), trail);
        CHECK(selector.next() == std::optional<ClauseId>{0});
    }

    SUBCASE("compaction rebuilds every bucket from the active clauses") {
        selector.add_starting_clause(make_clause(backend, 4, {{0}, {1}})); // id 0
        selector.add_starting_clause(make_clause(backend, 4, {{2}}));      // id 1, unit
        REQUIRE(drain(selector) == std::vector<ClauseId>{1});

        // Rewrite clause 0 in place, the way the solver applies a plan, and
        // deactivate the unit clause.
        selector.clause(0) = make_clause(backend, 4, {{2}, {3}});
        selector.set_clause_active(1, false);

        const CompactionTransform transform;
        selector.update_with_compaction(transform, trail);

        // The old bucket 0 is gone; the new bucket 2 wakes the clause.
        selector.update_with_propagation(make_equation_with(backend, 4, {0}), trail);
        CHECK(selector.next() == std::nullopt);

        selector.update_with_propagation(make_equation_with(backend, 4, {2}), trail);
        CHECK(selector.next() == std::optional<ClauseId>{0});
    }
}

} // namespace xorcle
