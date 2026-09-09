#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
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
    constexpr EquationRepr repr = Tag::value;
    Cyclic selector(4);
    const Trail trail(repr, 4);

    selector.add_starting_clause(make_clause(repr, 4, {{0}}));
    selector.add_starting_clause(make_clause(repr, 4, {{1}}));
    selector.add_starting_clause(make_clause(repr, 4, {{2}}));
    REQUIRE(selector.num_clauses() == 3);

    SUBCASE("a fresh selector starts at the most recently added clause") {
        // One full cycle, then quiescence.
        CHECK(drain(selector) == std::vector<ClauseId>{2, 0, 1});
    }

    SUBCASE("learning an equation refreshes the budget without moving the cursor") {
        CHECK(selector.next() == std::optional<ClauseId>{2});
        CHECK(selector.next() == std::optional<ClauseId>{0});
        selector.update_with_propagation(make_equation_with(repr, 4, {3}), trail);
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
    constexpr EquationRepr repr = Tag::value;
    LeadingTermWatchlist selector(4);
    Trail trail(repr, 4);

    SUBCASE("a unit clause is queued immediately, a non-unit one is not") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {1}}));
        CHECK(selector.next() == std::nullopt);

        selector.add_starting_clause(make_clause(repr, 4, {{2}}));
        CHECK(selector.next() == std::optional<ClauseId>{1});
        CHECK(selector.next() == std::nullopt);
    }

    SUBCASE("only the buckets a learned equation can reach are woken") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {3}})); // buckets 0 and 3
        selector.add_starting_clause(make_clause(repr, 4, {{1}, {3}})); // buckets 1 and 3
        selector.add_starting_clause(make_clause(repr, 4, {{2}, {3}})); // buckets 2 and 3

        // Learn x0 = 1 so column 0 becomes a pivot, then learn an equation
        // leading on column 2. Bucket 2 is the exact match; bucket 0 is a pivot
        // below it, so row reduction could lift such a row up to column 2. Bucket 1
        // is below but is not a pivot, so clause 1 stays asleep.
        trail.learn(make_equation_with(repr, 4, {0, 4}), ClauseId{0});
        selector.update_with_propagation(make_equation_with(repr, 4, {2}), trail);

        const std::vector<ClauseId> woken = drain(selector);
        CHECK(woken.size() == 2);
        CHECK(std::find(woken.begin(), woken.end(), ClauseId{1}) == woken.end());
    }

    SUBCASE("the work list is a deque: exact matches on top, lower pivots underneath") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {3}})); // id 0, buckets 0 and 3
        selector.add_starting_clause(make_clause(repr, 4, {{2}, {3}})); // id 1, buckets 2 and 3
        selector.add_starting_clause(make_clause(repr, 4, {{4}}));      // id 2, a unit clause

        // The unit clause is already queued. Learning an equation leading on
        // column 2 pushes bucket 2 to the top and bucket 0 -- a pivot below --
        // to the bottom, so the queued unit clause ends up between them.
        trail.learn(make_equation_with(repr, 4, {0, 4}), ClauseId{0});
        selector.update_with_propagation(make_equation_with(repr, 4, {2}), trail);

        CHECK(drain(selector) == std::vector<ClauseId>{1, 2, 0});
    }

    SUBCASE("an asserting clause is queued above every unit clause") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}}));      // id 0, unit
        selector.add_starting_clause(make_clause(repr, 4, {{1}}));      // id 1, unit
        selector.add_starting_clause(make_clause(repr, 4, {{2}, {3}})); // id 2, non-unit

        selector.add_asserting_clause(make_clause(repr, 4, {{3}, {4}}), trail,
                                      AssertingClauseInfo{.lbd = 1, .removed_levels = 2});
        CHECK(drain(selector) == std::vector<ClauseId>{3, 1, 0});
    }

    SUBCASE("inactive clauses are dropped as they are popped") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}}));
        selector.add_starting_clause(make_clause(repr, 4, {{1}}));
        selector.set_clause_active(0, false);
        CHECK(drain(selector) == std::vector<ClauseId>{1});
    }

    SUBCASE("moving a watch moves the clause between buckets") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {1}, {2}}));
        Clause& clause = selector.clause(0);
        REQUIRE(clause.rows() == 3);

        // The propagator writes the new watch first, then reports it.
        const ClauseRowIndex old_watch = clause.watched1;
        clause.watched1 = 2;
        selector.update_with_new_watch(0, old_watch, WatchType::one, trail);

        // Bucket 1 no longer holds the clause, so an equation leading there
        // wakes nothing; bucket 2 does.
        selector.update_with_propagation(make_equation_with(repr, 4, {1}), trail);
        CHECK(selector.next() == std::nullopt);

        selector.update_with_propagation(make_equation_with(repr, 4, {2}), trail);
        CHECK(selector.next() == std::optional<ClauseId>{0});
    }

    SUBCASE("compaction rebuilds every bucket from the active clauses") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {1}})); // id 0
        selector.add_starting_clause(make_clause(repr, 4, {{2}}));      // id 1, unit
        REQUIRE(drain(selector) == std::vector<ClauseId>{1});

        // Rewrite clause 0 in place, the way the solver applies a plan, and
        // deactivate the unit clause.
        selector.clause(0) = make_clause(repr, 4, {{2}, {3}});
        selector.set_clause_active(1, false);

        const CompactionTransform transform;
        selector.update_with_compaction(transform, trail);

        // The old bucket 0 is gone; the new bucket 2 wakes the clause.
        selector.update_with_propagation(make_equation_with(repr, 4, {0}), trail);
        CHECK(selector.next() == std::nullopt);

        selector.update_with_propagation(make_equation_with(repr, 4, {2}), trail);
        CHECK(selector.next() == std::optional<ClauseId>{0});
    }
}

TEST_CASE_TEMPLATE("clause selector: per-clause metadata", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Cyclic selector(4);
    const Trail trail(repr, 4);

    SUBCASE("an input clause is unlearned and starts in local, with no LBD recorded") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}}));
        CHECK_FALSE(selector.is_clause_learned(0));
        CHECK(selector.clause_lbd(0) == unset_clause_lbd);
        CHECK(selector.clause_tier(0) == ClauseTier::local);
        CHECK_FALSE(selector.is_clause_used_since_deletion(0));
        CHECK(selector.clause_unused_conflicts(0) == 0);
    }

    SUBCASE("a learned clause takes its LBD and the tier that LBD names") {
        const ClauseTierConfig& cutoffs = selector.tier_config();
        REQUIRE(cutoffs.core_lbd == 2);
        REQUIRE(cutoffs.tier2_lbd == 6);

        const std::size_t lbds[] = {cutoffs.core_lbd, cutoffs.core_lbd + 1, cutoffs.tier2_lbd,
                                    cutoffs.tier2_lbd + 1};
        const ClauseTier tiers[] = {ClauseTier::core, ClauseTier::tier2, ClauseTier::tier2,
                                    ClauseTier::local};
        for (std::size_t index = 0; index < std::size(lbds); ++index) {
            selector.add_asserting_clause(
                make_clause(repr, 4, {{index}}), trail,
                AssertingClauseInfo{.lbd = lbds[index], .removed_levels = 0});
            CHECK(selector.is_clause_learned(index));
            CHECK(selector.clause_lbd(index) == lbds[index]);
            CHECK(selector.clause_tier(index) == tiers[index]);
        }
    }

    SUBCASE("the cutoffs are configurable before the first clause") {
        selector.set_tier_config(ClauseTierConfig{.core_lbd = 1, .tier2_lbd = 1});
        selector.add_asserting_clause(make_clause(repr, 4, {{0}}), trail,
                                      AssertingClauseInfo{.lbd = 2, .removed_levels = 0});
        CHECK(selector.clause_tier(0) == ClauseTier::local);
    }

    SUBCASE("the mutators are independent of the flags beside them") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}}));
        selector.add_starting_clause(make_clause(repr, 4, {{1}}));

        selector.set_clause_used_since_deletion(1, true);
        selector.set_clause_unused_conflicts(1, 2);
        selector.set_clause_tier(1, ClauseTier::local);
        selector.set_clause_lbd(1, 7);

        CHECK(selector.is_clause_used_since_deletion(1));
        CHECK(selector.clause_unused_conflicts(1) == 2);
        CHECK(selector.clause_tier(1) == ClauseTier::local);
        CHECK(selector.clause_lbd(1) == 7);
        CHECK(selector.is_clause_active(1));

        CHECK_FALSE(selector.is_clause_used_since_deletion(0));
        CHECK(selector.clause_unused_conflicts(0) == 0);
        CHECK(selector.clause_tier(0) == ClauseTier::local);
        CHECK(selector.clause_lbd(0) == unset_clause_lbd);
    }

    SUBCASE("metadata is id-indexed, so compaction leaves it alone") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}}));
        selector.add_asserting_clause(make_clause(repr, 4, {{1}}), trail,
                                      AssertingClauseInfo{.lbd = 5, .removed_levels = 0});
        selector.set_clause_used_since_deletion(1, true);
        selector.set_clause_unused_conflicts(1, 3);
        selector.set_clause_active(0, false);

        const CompactionTransform transform;
        selector.update_with_compaction(transform, trail);

        CHECK_FALSE(selector.is_clause_learned(0));
        CHECK(selector.is_clause_learned(1));
        CHECK(selector.clause_lbd(1) == 5);
        CHECK(selector.clause_tier(1) == ClauseTier::tier2);
        CHECK(selector.is_clause_used_since_deletion(1));
        CHECK(selector.clause_unused_conflicts(1) == 3);
    }
}

} // namespace xorcle
