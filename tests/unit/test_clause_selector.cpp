#include <doctest/doctest.h>

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
#include "heuristics/selection/exact_watchlist.h"
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

/// @brief Learn an equation the way the solver does, then report the row the trail
/// stores for it: at a new level for a decision, at the top level otherwise.
void learn(ExactWatchlist& selector, Trail& trail, std::initializer_list<std::size_t> bits,
           bool decision) {
    const Equation eqn = make_equation_with(EquationRepr::dense, trail.num_vars(), bits);
    if (decision) {
        trail.learn(eqn, std::nullopt, trail.level() + 1);
    } else {
        trail.learn(eqn, ClauseId{0}, trail.level());
    }
    const Equation& row = trail.span().row(trail.trail_idx_to_row_idx().back());
    if (decision) {
        selector.update_with_decision(row, trail);
    } else {
        selector.update_with_propagation(row, trail);
    }
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

    SUBCASE("an attempt budget stops a run short of quiescence") {
        selector.set_attempt_budget(2);
        CHECK(drain(selector) == std::vector<ClauseId>{2, 0});
        CHECK(selector.last_stop_was_budget());

        // The next run gets the whole budget again, from where the cut left off.
        selector.set_attempt_budget(2);
        CHECK(drain(selector) == std::vector<ClauseId>{1, 2});
        CHECK(selector.last_stop_was_budget());
    }

    SUBCASE("a learned equation refreshes the attempt budget too") {
        selector.set_attempt_budget(2);
        CHECK(selector.next() == std::optional<ClauseId>{2});
        CHECK(selector.next() == std::optional<ClauseId>{0});
        selector.update_with_propagation(make_equation_with(repr, 4, {3}), trail);
        // The budget bounds a fruitless stretch, so the stretch that just
        // produced an equation does not count against the next one.
        CHECK(drain(selector) == std::vector<ClauseId>{1, 2});
        CHECK(selector.last_stop_was_budget());
    }

    SUBCASE("a budget as long as the database leaves quiescence the reason") {
        selector.set_attempt_budget(3);
        CHECK(drain(selector) == std::vector<ClauseId>{2, 0, 1});
        CHECK_FALSE(selector.last_stop_was_budget());
    }

    SUBCASE("quiescence inside the budget is not a budget stop") {
        selector.set_attempt_budget(100);
        CHECK(drain(selector) == std::vector<ClauseId>{2, 0, 1});
        CHECK_FALSE(selector.last_stop_was_budget());
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

TEST_CASE("exact watchlist: triggers, hints and snapshots") {
    constexpr EquationRepr repr = EquationRepr::dense;
    ExactWatchlist selector(4);
    Trail trail(repr, 4);

    SUBCASE("a row leading on a watch's column wakes it, and a dead first watch is a zero trigger") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {1}}));
        selector.add_starting_clause(make_clause(repr, 4, {{2}, {3}}));
        CHECK(selector.next() == std::nullopt);

        learn(selector, trail, {0}, false);
        CHECK(selector.counters().touches == 1);
        const std::optional<ClauseSelection> selection = selector.next_with_hint();
        REQUIRE(selection.has_value());
        CHECK(selection->id == 0);
        CHECK(selection->trigger_hint == std::optional<WatchType>{WatchType::zero});
        CHECK(selector.next() == std::nullopt);
        CHECK(selector.counters().triggers == 1);
    }

    SUBCASE("a first watch moving onto the second's column is fixed up into a one trigger") {
        // `A` = x0 + x2 and `B` = x1 + x2. Learning x0 + x1 moves `A` to x1 + x2,
        // which `B` then loses to: the watches became equal modulo the span.
        selector.add_starting_clause(make_clause(repr, 4, {{0, 2}, {1, 2}}));
        learn(selector, trail, {0, 1}, false);
        CHECK(selector.counters().fixups == 1);
        const std::optional<ClauseSelection> selection = selector.next_with_hint();
        REQUIRE(selection.has_value());
        CHECK(selection->trigger_hint == std::optional<WatchType>{WatchType::one});
    }

    SUBCASE("a clause a watch satisfies is not queued") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {1}}));
        learn(selector, trail, {1, 4}, false); // the second watch reduces to `0 = 1`
        learn(selector, trail, {0}, false);    // and the first to `0 = 0`
        CHECK(selector.next() == std::nullopt);
        CHECK(selector.counters().triggers == 0);
    }

    SUBCASE("a unit clause waits in the queue, with no hint, until it is satisfied") {
        selector.add_starting_clause(make_clause(repr, 4, {{2}}));
        const std::optional<ClauseSelection> selection = selector.next_with_hint();
        REQUIRE(selection.has_value());
        CHECK(selection->trigger_hint == std::nullopt);
        learn(selector, trail, {2, 4}, false);
        CHECK(selector.next() == std::nullopt);
    }

    SUBCASE("a backjump copies a swept cell back and drops what was queued") {
        // `A` = x0 + x1 and `B` = x2.
        selector.add_starting_clause(make_clause(repr, 4, {{0, 1}, {2}}));
        learn(selector, trail, {0}, true); // `A` becomes x1, saved first
        CHECK(selector.counters().saves == 1);
        learn(selector, trail, {1}, false); // `A` becomes `0 = 0`
        REQUIRE(selector.counters().triggers == 1);

        trail.resize(1);
        selector.update_with_backjump(0, trail);
        CHECK(selector.counters().restores == 1);
        CHECK(selector.next() == std::nullopt);

        // Restored to x0 + x1, so x1 alone no longer reaches it and x0 does.
        learn(selector, trail, {1}, true);
        CHECK(selector.next() == std::nullopt);
        const std::size_t touches = selector.counters().touches;
        learn(selector, trail, {0}, false);
        CHECK(selector.counters().touches == touches + 1);
        CHECK(selector.next_with_hint()->trigger_hint == std::optional<WatchType>{WatchType::zero});
    }

    SUBCASE("a backjump rebuilds a cell whose watch moved, and queues it if it triggers") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {1}, {2}}));
        learn(selector, trail, {1}, true); // `B` = x1 dies
        REQUIRE(selector.next_with_hint()->trigger_hint == std::optional<WatchType>{WatchType::one});

        // The propagator writes the new watch first, then reports it.
        Clause& clause = selector.clause(0);
        clause.watched1 = 2;
        selector.update_with_new_watch(0, 1, WatchType::one, trail);
        CHECK(selector.next() == std::nullopt);

        // Back at the root the new pair x0, x2 is independent: nothing triggers.
        trail.resize(1);
        selector.update_with_backjump(0, trail);
        CHECK(selector.counters().rebuilds == 2);
        CHECK(selector.next() == std::nullopt);
        learn(selector, trail, {2}, true);
        CHECK(selector.next_with_hint()->trigger_hint == std::optional<WatchType>{WatchType::one});
    }

    SUBCASE("a satisfied clause's second cell is left in its bucket, and swept once unsatisfied") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {1}}));
        learn(selector, trail, {0, 4}, true); // `A` reduces to `0 = 1` at level 1
        learn(selector, trail, {1}, true);    // and `B`'s column becomes a pivot at level 2
        CHECK(selector.counters().skips == 1);
        const std::size_t touches = selector.counters().touches;
        CHECK(selector.next() == std::nullopt);

        // Still satisfied at level 1, where `B` leads on a column again.
        trail.resize(2);
        selector.update_with_backjump(1, trail);
        learn(selector, trail, {1}, true);
        CHECK(selector.counters().skips == 2);

        // At the root `A` is x0 again, and `B` is still bucketed on x1.
        trail.resize(1);
        selector.update_with_backjump(0, trail);
        learn(selector, trail, {1}, true);
        CHECK(selector.counters().touches == touches + 1);
        CHECK(selector.counters().skips == 2);
        CHECK(selector.next_with_hint()->trigger_hint == std::optional<WatchType>{WatchType::one});
    }

    SUBCASE("a first cell whose second is `0 = 1` is left in its bucket until a backjump") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {1}}));
        learn(selector, trail, {1, 4}, true); // `B` reduces to `0 = 1`
        learn(selector, trail, {0}, true);    // and would make `A` `0 = 0`
        CHECK(selector.counters().skips == 1);
        CHECK(selector.next() == std::nullopt);

        trail.resize(1);
        selector.update_with_backjump(0, trail);
        learn(selector, trail, {0}, true);
        CHECK(selector.counters().skips == 1);
        CHECK(selector.next_with_hint()->trigger_hint == std::optional<WatchType>{WatchType::zero});
    }

    SUBCASE("stale bucket entries are cleaned up, and the live ones still wake") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {1}, {2}, {3}}));
        Clause& clause = selector.clause(0);
        // Each move rebuilds `B` into another bucket and leaves the old entry stale.
        for (std::size_t move = 0; move < 20; ++move) {
            const ClauseRowIdx old_watch = clause.watched1;
            clause.watched1 = 1 + (clause.watched1 % 3);
            selector.update_with_new_watch(0, old_watch, WatchType::one, trail);
        }
        CHECK(selector.counters().cleanups >= 1);
        CHECK(selector.counters().cleanup_dropped >= 1);
        // Two live cells over five columns: never more than 2 * 2 + 5, plus the push.
        CHECK(selector.counters().peak_bucket_entries <= 10);

        learn(selector, trail, {static_cast<std::size_t>(clause.watched1)}, true);
        CHECK(selector.next_with_hint()->trigger_hint == std::optional<WatchType>{WatchType::one});
    }

    SUBCASE("an asserting clause unit after its backjump is queued at once") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {3}}));
        learn(selector, trail, {1}, false);
        selector.add_asserting_clause(make_clause(repr, 4, {{1, 2}, {1}}), trail,
                                      AssertingClauseInfo{.lbd = 1, .removed_levels = 1});
        const std::optional<ClauseSelection> selection = selector.next_with_hint();
        REQUIRE(selection.has_value());
        CHECK(selection->id == 1);
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
        CHECK(selector.clause_unused_passes(0) == 0);
    }

    SUBCASE("a learned clause takes its LBD and the tier that LBD names") {
        const ClauseTierConfig& cutoffs = selector.tier_config();
        REQUIRE(cutoffs.core_lbd == 2);
        REQUIRE(cutoffs.secondary_lbd == 6);

        const std::size_t lbds[] = {cutoffs.core_lbd, cutoffs.core_lbd + 1, cutoffs.secondary_lbd,
                                    cutoffs.secondary_lbd + 1};
        const ClauseTier tiers[] = {ClauseTier::core, ClauseTier::secondary, ClauseTier::secondary,
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
        selector.set_tier_config(ClauseTierConfig{.core_lbd = 1, .secondary_lbd = 1});
        selector.add_asserting_clause(make_clause(repr, 4, {{0}}), trail,
                                      AssertingClauseInfo{.lbd = 2, .removed_levels = 0});
        CHECK(selector.clause_tier(0) == ClauseTier::local);
    }

    SUBCASE("the mutators are independent of the flags beside them") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}}));
        selector.add_starting_clause(make_clause(repr, 4, {{1}}));

        selector.set_clause_used_since_deletion(1, true);
        selector.set_clause_unused_passes(1, 2);
        selector.set_clause_tier(1, ClauseTier::local);
        selector.set_clause_lbd(1, 7);

        CHECK(selector.is_clause_used_since_deletion(1));
        CHECK(selector.clause_unused_passes(1) == 2);
        CHECK(selector.clause_tier(1) == ClauseTier::local);
        CHECK(selector.clause_lbd(1) == 7);
        CHECK(selector.is_clause_active(1));

        CHECK_FALSE(selector.is_clause_used_since_deletion(0));
        CHECK(selector.clause_unused_passes(0) == 0);
        CHECK(selector.clause_tier(0) == ClauseTier::local);
        CHECK(selector.clause_lbd(0) == unset_clause_lbd);
    }

    SUBCASE("metadata is id-indexed, so compaction leaves it alone") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}}));
        selector.add_asserting_clause(make_clause(repr, 4, {{1}}), trail,
                                      AssertingClauseInfo{.lbd = 5, .removed_levels = 0});
        selector.set_clause_used_since_deletion(1, true);
        selector.set_clause_unused_passes(1, 3);
        selector.set_clause_active(0, false);

        const CompactionTransform transform;
        selector.update_with_compaction(transform, trail);

        CHECK_FALSE(selector.is_clause_learned(0));
        CHECK(selector.is_clause_learned(1));
        CHECK(selector.clause_lbd(1) == 5);
        CHECK(selector.clause_tier(1) == ClauseTier::secondary);
        CHECK(selector.is_clause_used_since_deletion(1));
        CHECK(selector.clause_unused_passes(1) == 3);
    }
}

} // namespace xorcle
