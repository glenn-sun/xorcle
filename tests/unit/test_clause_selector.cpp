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

/// @brief The id of the next clause a selector hands out.
std::optional<ClauseId> next_id(ClauseSelector& selector) {
    const std::optional<ClauseSelection> selection = selector.next();
    if (!selection.has_value()) {
        return std::nullopt;
    }
    return selection->id;
}

/// @brief Drain a selector, collecting the ids it hands out.
std::vector<ClauseId> drain(ClauseSelector& selector) {
    std::vector<ClauseId> out;
    while (const std::optional<ClauseId> id = next_id(selector)) {
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

TEST_CASE("make_clause_selector: dispatches on the name") {
    CHECK(dynamic_cast<Cyclic*>(make_clause_selector(ClauseSelectorName::cyclic, 4).get()) !=
          nullptr);
    CHECK(dynamic_cast<ExactWatchlist*>(
              make_clause_selector(ClauseSelectorName::exact_watchlist, 4).get()) != nullptr);
}

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
        CHECK(next_id(selector) == std::optional<ClauseId>{2});
        CHECK(next_id(selector) == std::optional<ClauseId>{0});
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
        CHECK(next_id(selector) == std::optional<ClauseId>{2});
        CHECK(next_id(selector) == std::optional<ClauseId>{0});
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
        CHECK(next_id(selector) == std::optional<ClauseId>{1});
        CHECK(next_id(selector) == std::optional<ClauseId>{2});
    }

    SUBCASE("an inactive or out-of-range hint is ignored") {
        selector.set_clause_active(1, false);
        selector.update_with_decision_clause(ClauseId{1});
        CHECK(next_id(selector) == std::optional<ClauseId>{2});

        selector.update_with_decision_clause(ClauseId{99});
        CHECK(next_id(selector) == std::optional<ClauseId>{0});
    }

    SUBCASE("compaction moves the cursor onto an active clause") {
        const CompactionTransform transform;
        selector.set_clause_active(2, false);
        selector.set_clause_active(0, false);
        selector.update_with_compaction(transform, trail);
        CHECK(next_id(selector) == std::optional<ClauseId>{1});
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
        const std::optional<ClauseSelection> selection = selector.next();
        REQUIRE(selection.has_value());
        CHECK(selection->id == 0);
        CHECK(selection->trigger_hint == std::optional<WatchType>{WatchType::zero});
        CHECK(selector.next() == std::nullopt);
    }

    SUBCASE("a first watch moving onto the second's column is fixed up into a one trigger") {
        // `A` = x0 + x2 and `B` = x1 + x2. Learning x0 + x1 moves `A` to x1 + x2,
        // which `B` then loses to: the watches became equal modulo the span.
        selector.add_starting_clause(make_clause(repr, 4, {{0, 2}, {1, 2}}));
        learn(selector, trail, {0, 1}, false);
        const std::optional<ClauseSelection> selection = selector.next();
        REQUIRE(selection.has_value());
        CHECK(selection->trigger_hint == std::optional<WatchType>{WatchType::one});
        CHECK(selector.counters().fixups > 0);
    }

    SUBCASE("a trigger's partner a sweep has yet to reach is swept ahead, and may satisfy it") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {1}}));
        learn(selector, trail, {0}, false);    // the first watch, swept first, reduces to `0 = 0`
        learn(selector, trail, {1, 4}, false); // and the second to `0 = 1`
        CHECK(selector.next() == std::nullopt);
        CHECK(selector.counters().touches_ahead > 0);
    }

    SUBCASE("a unit clause waits in the queue, with no hint, until it is satisfied") {
        selector.add_starting_clause(make_clause(repr, 4, {{2}}));
        const std::optional<ClauseSelection> selection = selector.next();
        REQUIRE(selection.has_value());
        CHECK(selection->trigger_hint == std::nullopt);
        learn(selector, trail, {2, 4}, false);
        CHECK(selector.next() == std::nullopt);
    }

    SUBCASE("the side queue goes first, then the oldest sweep, which pauses at each trigger") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {1}}));
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {2}}));
        selector.add_starting_clause(make_clause(repr, 4, {{3}, {1}}));
        learn(selector, trail, {0}, false);
        learn(selector, trail, {3}, false);
        selector.add_starting_clause(make_clause(repr, 4, {{1, 2}}));

        CHECK(selector.next()->id == 3);
        CHECK(selector.next()->id == 0);
        CHECK(selector.next()->id == 1);
        CHECK(selector.next()->id == 2);
        CHECK(selector.next() == std::nullopt);
    }

    SUBCASE("a backjump drops a paused sweep, and the cells it never reached wake again") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {1}}));
        learn(selector, trail, {0}, true);
        trail.resize(1);
        selector.update_with_backjump(0, trail);
        CHECK(selector.counters().sweeps_dropped > 0);
        CHECK(selector.counters().sweeps_dropped_unreached > 0);
        CHECK(selector.next() == std::nullopt);

        learn(selector, trail, {0}, true);
        CHECK(selector.next()->trigger_hint == std::optional<WatchType>{WatchType::zero});
    }

    SUBCASE("a backjump copies a swept cell back and drops what was queued") {
        // `A` = x0 + x1 and `B` = x2.
        selector.add_starting_clause(make_clause(repr, 4, {{0, 1}, {2}}));
        learn(selector, trail, {0}, true); // `A` becomes x1, saved first
        CHECK(selector.next() == std::nullopt);
        learn(selector, trail, {1}, false); // `A` becomes `0 = 0`
        REQUIRE(selector.next()->trigger_hint ==
                std::optional<WatchType>{WatchType::zero});

        trail.resize(1);
        selector.update_with_backjump(0, trail);
        CHECK(selector.counters().restores > 0);
        CHECK(selector.next() == std::nullopt);

        // Restored to x0 + x1, so x1 alone no longer reaches it and x0 does.
        learn(selector, trail, {1}, true);
        CHECK(selector.next() == std::nullopt);
        learn(selector, trail, {0}, false);
        CHECK(selector.next()->trigger_hint == std::optional<WatchType>{WatchType::zero});
    }

    SUBCASE("a backjump rebuilds a cell whose watch moved, and queues it if it triggers") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {1}, {2}}));
        learn(selector, trail, {1}, true); // `B` = x1 dies
        REQUIRE(selector.next()->trigger_hint == std::optional<WatchType>{WatchType::one});

        // The propagator writes the new watch first, then reports it.
        Clause& clause = selector.clause(0);
        clause.watched1 = 2;
        selector.update_with_new_watch(0, 1, WatchType::one, trail);
        CHECK(selector.next() == std::nullopt);

        // Back at the root the new pair x0, x2 is independent: nothing triggers.
        trail.resize(1);
        selector.update_with_backjump(0, trail);
        CHECK(selector.next() == std::nullopt);
        learn(selector, trail, {2}, true);
        CHECK(selector.next()->trigger_hint == std::optional<WatchType>{WatchType::one});
    }

    SUBCASE("a satisfied clause's second cell is left in its bucket, and swept once unsatisfied") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {1}}));
        learn(selector, trail, {0, 4}, true); // `A` reduces to `0 = 1` at level 1
        CHECK(selector.next() == std::nullopt);
        learn(selector, trail, {1}, true); // and `B`'s column becomes a pivot at level 2
        CHECK(selector.next() == std::nullopt);
        CHECK(selector.counters().skips > 0);

        // Still satisfied at level 1, where `B` leads on a column again.
        trail.resize(2);
        selector.update_with_backjump(1, trail);
        learn(selector, trail, {1}, true);
        CHECK(selector.next() == std::nullopt);

        // At the root `A` is x0 again, and `B` is still bucketed on x1.
        trail.resize(1);
        selector.update_with_backjump(0, trail);
        learn(selector, trail, {1}, true);
        CHECK(selector.next()->trigger_hint == std::optional<WatchType>{WatchType::one});
    }

    SUBCASE("a first cell whose second is `0 = 1` is left in its bucket until a backjump") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {1}}));
        learn(selector, trail, {1, 4}, true); // `B` reduces to `0 = 1`
        CHECK(selector.next() == std::nullopt);
        learn(selector, trail, {0}, true); // and would make `A` `0 = 0`
        CHECK(selector.next() == std::nullopt);
        CHECK(selector.counters().skips > 0);

        trail.resize(1);
        selector.update_with_backjump(0, trail);
        learn(selector, trail, {0}, true);
        CHECK(selector.next()->trigger_hint == std::optional<WatchType>{WatchType::zero});
    }

    SUBCASE("stale bucket entries are cleaned up, and the live ones still wake") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {1}, {2}, {3}}));
        Clause& clause = selector.clause(0);
        // Each move rebuilds `B` into another bucket and leaves the old entry stale.
        for (std::size_t move = 0; move < 20; ++move) {
            const ClauseRowIdx old_watch_row_idx = clause.watched1;
            clause.watched1 = 1 + (clause.watched1 % 3);
            selector.update_with_new_watch(0, old_watch_row_idx, WatchType::one, trail);
        }
        CHECK(selector.counters().cleanups >= 1);
        CHECK(selector.counters().cleanup_dropped >= 1);
        // Two live cells over five columns: never more than 2 * 2 + 5, plus the push.
        CHECK(selector.counters().peak_bucket_entries <= 10);

        learn(selector, trail, {static_cast<std::size_t>(clause.watched1)}, true);
        CHECK(selector.next()->trigger_hint == std::optional<WatchType>{WatchType::one});
    }

    SUBCASE("a clause deactivated after it triggered is not handed out") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {1}}));
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {2}}));
        learn(selector, trail, {0}, false);
        selector.set_clause_active(0, false);
        CHECK(next_id(selector) == std::optional<ClauseId>{1});
        CHECK(selector.next() == std::nullopt);
    }

    SUBCASE("a replacement the trail already makes unit is queued at once") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {3}}));
        learn(selector, trail, {1}, false);
        selector.add_asserting_clause(
            make_clause(repr, 4, {{1, 2}, {1}}), trail,
            AssertingClauseInfo{.lbd = 1, .removed_levels = 0, .is_replacement = true});
        CHECK(next_id(selector) == std::optional<ClauseId>{1});
        CHECK(selector.next() == std::nullopt);
    }

    SUBCASE("an asserting clause unit after its backjump is queued at once") {
        selector.add_starting_clause(make_clause(repr, 4, {{0}, {3}}));
        learn(selector, trail, {1}, false);
        selector.add_asserting_clause(make_clause(repr, 4, {{1, 2}, {1}}), trail,
                                      AssertingClauseInfo{.lbd = 1, .removed_levels = 1});
        const std::optional<ClauseSelection> selection = selector.next();
        REQUIRE(selection.has_value());
        CHECK(selection->id == 1);
    }
}

TEST_CASE("exact watchlist: watches past the first word") {
    constexpr EquationRepr repr = EquationRepr::dense;
    constexpr std::size_t num_vars = 130;
    ExactWatchlist selector(num_vars);
    Trail trail(repr, num_vars);
    selector.add_starting_clause(make_clause(repr, num_vars, {{100}, {3}}));
    selector.add_starting_clause(make_clause(repr, num_vars, {{64}, {127}}));
    selector.add_starting_clause(make_clause(repr, num_vars, {{129}, {65}}));
    CHECK(selector.next() == std::nullopt);

    // x100 kills one of the first clause's watches, in the second word.
    learn(selector, trail, {100}, true);
    const std::optional<ClauseSelection> first = selector.next();
    REQUIRE(first.has_value());
    CHECK(first->id == 0);
    CHECK(first->trigger_hint.has_value());
    CHECK(selector.next() == std::nullopt);

    // x64 + x127 makes the second clause's watches equal, across a word boundary.
    learn(selector, trail, {64, 127}, true);
    const std::optional<ClauseSelection> second = selector.next();
    REQUIRE(second.has_value());
    CHECK(second->id == 1);
    CHECK(second->trigger_hint.has_value());
    CHECK(selector.next() == std::nullopt);

    // Back at the root, the third clause, in the third word, still wakes.
    trail.resize(1);
    selector.update_with_backjump(0, trail);
    learn(selector, trail, {129}, true);
    CHECK(next_id(selector) == std::optional<ClauseId>{2});
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
        REQUIRE(cutoffs.core_lbd < cutoffs.secondary_lbd);

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
