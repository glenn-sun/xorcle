#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/dense/dense_gf2_vector.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra/sparse/sparse_gf2_vector.h"
#include "algebra_test_support.h"
#include "core/clause_database.h"
#include "core/clause_id.h"
#include "core/propagation.h"
#include "core/trail.h"
#include "core/watch.h"
#include "heuristics/compaction/compaction.h"
#include "heuristics/selection/clause_selector.h"
#include "heuristics/selection/cyclic.h"
#include "heuristics/selection/exact_watchlist.h"
#include "heuristics_test_support.h"
#include "util/index_set.h"

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
void learn(ClauseSelector& selector, Trail& trail, std::initializer_list<std::size_t> bits,
           bool decision) {
    const Equation eqn = make_equation_with(trail.span().repr(), trail.num_vars(), bits);
    if (decision) {
        trail.learn(eqn, std::nullopt, trail.level() + 1);
    } else {
        trail.learn(eqn, ClauseId{0}, trail.level());
    }
    const Equation& row = trail.span().row(trail.trail_idx_to_row_idx().back());
    if (decision) {
        selector.update_with_decision(row, trail, std::nullopt);
    } else {
        selector.update_with_new_equation(row, trail);
    }
}

/// @brief The exact watchlist over a tag's representation.
template <class Tag>
using ExactWatchlistOf = ExactWatchlistTemplate<
    std::conditional_t<Tag::value == EquationRepr::dense, DenseGF2Vector, SparseGF2Vector>>;

} // namespace

TEST_CASE("make_clause_selector: dispatches on the name and the representation") {
    const ClauseDatabase database;
    const auto make = [&database](ClauseSelectorName name, EquationRepr repr) {
        return make_clause_selector(name, repr, 4, database);
    };
    CHECK(dynamic_cast<Cyclic*>(make(ClauseSelectorName::cyclic, EquationRepr::dense).get()) !=
          nullptr);
    CHECK(dynamic_cast<ExactWatchlistTemplate<DenseGF2Vector>*>(
              make(ClauseSelectorName::exact_watchlist, EquationRepr::dense).get()) != nullptr);
    CHECK(dynamic_cast<ExactWatchlistTemplate<SparseGF2Vector>*>(
              make(ClauseSelectorName::exact_watchlist, EquationRepr::sparse).get()) != nullptr);
}

TEST_CASE_TEMPLATE("cyclic: round-robin order and a fixed point", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    ClauseDatabase database;
    Cyclic selector(database);
    const Trail trail(repr, 4);

    add_starting_clause(database, selector, make_clause(repr, 4, {{0}}));
    add_starting_clause(database, selector, make_clause(repr, 4, {{1}}));
    add_starting_clause(database, selector, make_clause(repr, 4, {{2}}));
    REQUIRE(database.num_clauses() == 3);

    SUBCASE("a fresh selector starts at the most recently added clause") {
        // One full cycle, then a fixed point.
        CHECK(drain(selector) == std::vector<ClauseId>{2, 0, 1});
    }

    SUBCASE("learning an equation refreshes the budget without moving the cursor") {
        CHECK(next_id(selector) == std::optional<ClauseId>{2});
        CHECK(next_id(selector) == std::optional<ClauseId>{0});
        selector.update_with_new_equation(make_equation_with(repr, 4, {3}), trail);
        // The budget restarts, so the cycle continues from clause 1 for a full
        // pass rather than stopping after one more.
        CHECK(drain(selector) == std::vector<ClauseId>{1, 2, 0});
    }

    SUBCASE("inactive clauses are skipped and still count against the budget") {
        deactivate_clause(database, selector, 0);
        CHECK(drain(selector) == std::vector<ClauseId>{2, 1});
    }

    SUBCASE("an attempt budget stops a run short of a fixed point") {
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
        selector.update_with_new_equation(make_equation_with(repr, 4, {3}), trail);
        // The budget bounds a fruitless stretch, so the stretch that just
        // produced an equation does not count against the next one.
        CHECK(drain(selector) == std::vector<ClauseId>{1, 2});
        CHECK(selector.last_stop_was_budget());
    }

    SUBCASE("a budget as long as the database leaves the fixed point the reason") {
        selector.set_attempt_budget(3);
        CHECK(drain(selector) == std::vector<ClauseId>{2, 0, 1});
        CHECK_FALSE(selector.last_stop_was_budget());
    }

    SUBCASE("a fixed point inside the budget is not a budget stop") {
        selector.set_attempt_budget(100);
        CHECK(drain(selector) == std::vector<ClauseId>{2, 0, 1});
        CHECK_FALSE(selector.last_stop_was_budget());
    }

    SUBCASE("a decision clause hint moves the cursor once") {
        selector.update_with_decision(make_equation_with(repr, 4, {3}), trail, ClauseId{1});
        CHECK(next_id(selector) == std::optional<ClauseId>{1});
        CHECK(next_id(selector) == std::optional<ClauseId>{2});
    }

    SUBCASE("an inactive hint starts the sweep at the next active clause") {
        deactivate_clause(database, selector, 1);
        selector.update_with_decision(make_equation_with(repr, 4, {3}), trail, ClauseId{1});
        CHECK(next_id(selector) == std::optional<ClauseId>{2});
    }

    SUBCASE("compaction moves the cursor onto an active clause") {
        deactivate_clause(database, selector, 2);
        deactivate_clause(database, selector, 0);
        selector.update_with_substitution({}, IndexSet(), trail);
        CHECK(next_id(selector) == std::optional<ClauseId>{1});
    }

    SUBCASE("an empty selector is immediately at a fixed point") {
        ClauseDatabase empty_database;
        Cyclic empty(empty_database);
        CHECK(empty.next() == std::nullopt);
    }
}

TEST_CASE_TEMPLATE("exact watchlist: triggers, hints and snapshots", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    ClauseDatabase database;
    ExactWatchlistOf<Tag> selector(4, database);
    Trail trail(repr, 4);

    SUBCASE("a row leading on a watch's column wakes it, and a dead first watch is a zero trigger") {
        add_starting_clause(database, selector, make_clause(repr, 4, {{0}, {1}}));
        add_starting_clause(database, selector, make_clause(repr, 4, {{2}, {3}}));
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
        add_starting_clause(database, selector, make_clause(repr, 4, {{0, 2}, {1, 2}}));
        learn(selector, trail, {0, 1}, false);
        const std::optional<ClauseSelection> selection = selector.next();
        REQUIRE(selection.has_value());
        CHECK(selection->trigger_hint == std::optional<WatchType>{WatchType::one});
    }

    SUBCASE("a trigger's partner a sweep has yet to reach is swept ahead, and may satisfy it") {
        add_starting_clause(database, selector, make_clause(repr, 4, {{0}, {1}}));
        learn(selector, trail, {0}, false);    // the first watch, swept first, reduces to `0 = 0`
        learn(selector, trail, {1, 4}, false); // and the second to `0 = 1`
        CHECK(selector.next() == std::nullopt);
    }

    SUBCASE("a unit clause waits in the queue, with no hint, until it is satisfied") {
        add_starting_clause(database, selector, make_clause(repr, 4, {{2}}));
        const std::optional<ClauseSelection> selection = selector.next();
        REQUIRE(selection.has_value());
        CHECK(selection->trigger_hint == std::nullopt);
        learn(selector, trail, {2, 4}, false);
        CHECK(selector.next() == std::nullopt);
    }

    SUBCASE("the side queue goes first, then the oldest sweep, which pauses at each trigger") {
        add_starting_clause(database, selector, make_clause(repr, 4, {{0}, {1}}));
        add_starting_clause(database, selector, make_clause(repr, 4, {{0}, {2}}));
        add_starting_clause(database, selector, make_clause(repr, 4, {{3}, {1}}));
        learn(selector, trail, {0}, false);
        learn(selector, trail, {3}, false);
        add_starting_clause(database, selector, make_clause(repr, 4, {{1, 2}}));

        CHECK(selector.next()->id == 3);
        CHECK(selector.next()->id == 0);
        CHECK(selector.next()->id == 1);
        CHECK(selector.next()->id == 2);
        CHECK(selector.next() == std::nullopt);
    }

    SUBCASE("a backjump drops a paused sweep, and the cells it never reached wake again") {
        add_starting_clause(database, selector, make_clause(repr, 4, {{0}, {1}}));
        learn(selector, trail, {0}, true);
        trail.resize(1);
        selector.update_with_backjump(0, trail);
        CHECK(selector.next() == std::nullopt);

        learn(selector, trail, {0}, true);
        CHECK(selector.next()->trigger_hint == std::optional<WatchType>{WatchType::zero});
    }

    SUBCASE("a backjump copies a swept cell back and drops what was queued") {
        // `A` = x0 + x1 and `B` = x2.
        add_starting_clause(database, selector, make_clause(repr, 4, {{0, 1}, {2}}));
        learn(selector, trail, {0}, true); // `A` becomes x1, saved first
        CHECK(selector.next() == std::nullopt);
        learn(selector, trail, {1}, false); // `A` becomes `0 = 0`
        REQUIRE(selector.next()->trigger_hint ==
                std::optional<WatchType>{WatchType::zero});

        trail.resize(1);
        selector.update_with_backjump(0, trail);
        CHECK(selector.next() == std::nullopt);

        // Restored to x0 + x1, so x1 alone no longer reaches it and x0 does.
        learn(selector, trail, {1}, true);
        CHECK(selector.next() == std::nullopt);
        learn(selector, trail, {0}, false);
        CHECK(selector.next()->trigger_hint == std::optional<WatchType>{WatchType::zero});
    }

    SUBCASE("a backjump rebuilds a cell whose watch moved, and queues it if it triggers") {
        add_starting_clause(database, selector, make_clause(repr, 4, {{0}, {1}, {2}}));
        learn(selector, trail, {1}, true); // `B` = x1 dies
        REQUIRE(selector.next()->trigger_hint == std::optional<WatchType>{WatchType::one});

        // The propagator writes the new watch first, then reports it.
        ClauseWatches& watches = database.record(0).watches;
        watches.watched1 = 2;
        report_new_watch(selector, 0, database.clause(0), watches, 1, WatchType::one, trail);
        CHECK(selector.next() == std::nullopt);

        // Back at the root the new pair x0, x2 is independent: nothing triggers.
        trail.resize(1);
        selector.update_with_backjump(0, trail);
        CHECK(selector.next() == std::nullopt);
        learn(selector, trail, {2}, true);
        CHECK(selector.next()->trigger_hint == std::optional<WatchType>{WatchType::one});
    }

    SUBCASE("a satisfied clause's second cell is left in its bucket, and swept once unsatisfied") {
        add_starting_clause(database, selector, make_clause(repr, 4, {{0}, {1}}));
        learn(selector, trail, {0, 4}, true); // `A` reduces to `0 = 1` at level 1
        CHECK(selector.next() == std::nullopt);
        learn(selector, trail, {1}, true); // and `B`'s column becomes a pivot at level 2
        CHECK(selector.next() == std::nullopt);

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
        add_starting_clause(database, selector, make_clause(repr, 4, {{0}, {1}}));
        learn(selector, trail, {1, 4}, true); // `B` reduces to `0 = 1`
        CHECK(selector.next() == std::nullopt);
        learn(selector, trail, {0}, true); // and would make `A` `0 = 0`
        CHECK(selector.next() == std::nullopt);

        trail.resize(1);
        selector.update_with_backjump(0, trail);
        learn(selector, trail, {0}, true);
        CHECK(selector.next()->trigger_hint == std::optional<WatchType>{WatchType::zero});
    }

    SUBCASE("stale bucket entries are cleaned up, and the live ones still wake") {
        add_starting_clause(database, selector, make_clause(repr, 4, {{0}, {1}, {2}, {3}}));
        ClauseWatches& watches = database.record(0).watches;
        // Each move rebuilds `B` into another bucket and leaves the old entry stale.
        for (std::size_t move = 0; move < 20; ++move) {
            const ClauseRowIdx old_watch_row_idx = watches.watched1;
            watches.watched1 = 1 + (watches.watched1 % 3);
            report_new_watch(selector, 0, database.clause(0), watches, old_watch_row_idx,
                             WatchType::one, trail);
        }

        learn(selector, trail, {static_cast<std::size_t>(watches.watched1)}, true);
        CHECK(selector.next()->trigger_hint == std::optional<WatchType>{WatchType::one});
    }

    SUBCASE("a clause deactivated after it triggered is not handed out") {
        add_starting_clause(database, selector, make_clause(repr, 4, {{0}, {1}}));
        add_starting_clause(database, selector, make_clause(repr, 4, {{0}, {2}}));
        learn(selector, trail, {0}, false);
        deactivate_clause(database, selector, 0);
        CHECK(next_id(selector) == std::optional<ClauseId>{1});
        CHECK(selector.next() == std::nullopt);
    }

    SUBCASE("a replacement the trail already makes unit is queued at once") {
        add_starting_clause(database, selector, make_clause(repr, 4, {{0}, {3}}));
        learn(selector, trail, {1}, false);
        add_learned_clause(
            database, selector, make_clause(repr, 4, {{1, 2}, {1}}), trail,
            AssertingClauseInfo{.lbd = 1, .removed_levels = 0, .is_replacement = true});
        CHECK(next_id(selector) == std::optional<ClauseId>{1});
        CHECK(selector.next() == std::nullopt);
    }

    SUBCASE("an asserting clause unit after its backjump is queued at once") {
        add_starting_clause(database, selector, make_clause(repr, 4, {{0}, {3}}));
        learn(selector, trail, {1}, false);
        add_learned_clause(database, selector, make_clause(repr, 4, {{1, 2}, {1}}), trail,
                             AssertingClauseInfo{.lbd = 1, .removed_levels = 1});
        const std::optional<ClauseSelection> selection = selector.next();
        REQUIRE(selection.has_value());
        CHECK(selection->id == 1);
    }
}

TEST_CASE_TEMPLATE("exact watchlist: the propagator reads and writes the cells", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    constexpr std::size_t num_vars = 6;
    ClauseDatabase database;
    ExactWatchlistOf<Tag> selector(num_vars, database);
    Trail trail(repr, num_vars);
    Propagator propagator(repr, num_vars);
    std::size_t num_watch_updates = 0;
    Equation reduced(repr, num_vars);
    ReduceResult result = ReduceResult::eq_true();

    SUBCASE("a one trigger leaves `A` standing, which is exact") {
        add_starting_clause(database, selector, make_clause(repr, num_vars, {{0, 2}, {1}}));
        learn(selector, trail, {0, 3}, false); // `A` becomes x2 + x3
        learn(selector, trail, {1}, false);    // `B` dies
        const std::optional<ClauseSelection> selection = selector.next();
        REQUIRE(selection.has_value());
        REQUIRE(selection->trigger_hint == std::optional<WatchType>{WatchType::one});

        CHECK(selector.copy_reduced_companion(0, WatchType::one, reduced, result) ==
              HeldReduction::exact);
        CHECK(bits_of(reduced) == std::vector<std::size_t>{2, 3});
        CHECK(result == ReduceResult::leading(2));

        Equation* implied = propagator.unit_propagate_one_clause(
            0, database.clause(0), database.record(0).watches, trail, selection->trigger_hint,
            num_watch_updates, selector);
        REQUIRE(implied != nullptr);
        CHECK(bits_of(*implied) == std::vector<std::size_t>{2, 3, num_vars});
    }

    SUBCASE("a zero trigger leaves `B` standing, which is congruent and stays a cell") {
        // `A` = x0 + x4 and `B` = x2 + x3. The first row moves `A` to x2 + x4 + x5,
        // which is added into `B`: x3 + x4 + x5.
        add_starting_clause(database, selector, make_clause(repr, num_vars, {{0, 4}, {2, 3}, {5}}));
        learn(selector, trail, {0, 2, 5}, false);
        CHECK(selector.next() == std::nullopt);
        // These two put `A` in the span, and reduce the second watch itself to x3.
        learn(selector, trail, {4, 5}, false);
        learn(selector, trail, {2}, false);
        std::optional<ClauseSelection> selection = selector.next();
        REQUIRE(selection.has_value());
        REQUIRE(selection->trigger_hint == std::optional<WatchType>{WatchType::zero});

        CHECK(selector.copy_reduced_companion(0, WatchType::zero, reduced, result) ==
              HeldReduction::congruent);
        CHECK(bits_of(reduced) == std::vector<std::size_t>{3, 4, 5});
        CHECK(result == ReduceResult::leading(3));

        // The first watch moves to x5, and `B` stays as it was read.
        const Clause& clause = database.clause(0);
        ClauseWatches& watches = database.record(0).watches;
        CHECK(propagator.unit_propagate_one_clause(0, clause, watches, trail,
                                                   selection->trigger_hint, num_watch_updates,
                                                   selector) == nullptr);
        CHECK(watches.watched0 == 2);
        CHECK(num_watch_updates == 1);
        CHECK(selector.next() == std::nullopt);

        // With x5 in the span nothing is left to watch, and what the clause
        // propagates is the second watch's own reduction.
        learn(selector, trail, {5}, false);
        selection = selector.next();
        REQUIRE(selection.has_value());
        REQUIRE(selection->trigger_hint == std::optional<WatchType>{WatchType::zero});
        Equation* implied = propagator.unit_propagate_one_clause(
            0, clause, watches, trail, selection->trigger_hint, num_watch_updates, selector);
        REQUIRE(implied != nullptr);
        CHECK(bits_of(*implied) == std::vector<std::size_t>{3, num_vars});
    }

    SUBCASE("a moved second watch's cell wakes on its own column") {
        add_starting_clause(database, selector, make_clause(repr, num_vars, {{0}, {1}, {2, 3}}));
        learn(selector, trail, {2, 4}, false); // the third row will reduce to x3 + x4
        learn(selector, trail, {1}, false);    // `B` dies
        std::optional<ClauseSelection> selection = selector.next();
        REQUIRE(selection.has_value());
        REQUIRE(selection->trigger_hint == std::optional<WatchType>{WatchType::one});
        ClauseWatches& watches = database.record(0).watches;
        CHECK(propagator.unit_propagate_one_clause(0, database.clause(0), watches, trail,
                                                   selection->trigger_hint, num_watch_updates,
                                                   selector) == nullptr);
        CHECK(watches.watched1 == 2);
        CHECK(selector.next() == std::nullopt);

        learn(selector, trail, {3, 4}, false); // the new `B`, keyed on x3, dies
        selection = selector.next();
        REQUIRE(selection.has_value());
        CHECK(selection->trigger_hint == std::optional<WatchType>{WatchType::one});
    }
}

TEST_CASE_TEMPLATE("exact watchlist: a substitution rebuilds only the clauses it rewrote", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    ClauseDatabase database;
    ExactWatchlistOf<Tag> selector(5, database);
    Trail trail(repr, 5);

    add_starting_clause(database, selector, make_clause(repr, 5, {{0, 1}, {2}})); // carries x0
                        add_starting_clause(database, selector, make_clause(repr, 5, {{1}, {2}}));
    add_starting_clause(database, selector, make_clause(repr, 5, {{3}}));
    add_starting_clause(database, selector, make_clause(repr, 5, {{1}, {2}, {4}}));
    REQUIRE(selector.next()->id == 2);
    database.record(3).watches.watched1 = 2;
    report_new_watch(selector, 3, database.clause(3), database.record(3).watches, 1,
                     WatchType::one, trail);
    learn(selector, trail, {3, 5}, false); // x3 = 1 satisfies the unit
    learn(selector, trail, {0}, false);    // x0 = 0
    REQUIRE(selector.next() == std::nullopt);

    // What the solver does: the clause carrying x0 is rewritten, the satisfied one
    // goes, the moved watch stays where it is, and the trail is emptied.
    database.replace_clause(0, make_clause(repr, 5, {{1}, {2}}));
    deactivate_clause(database, selector, 2);
    trail = Trail(repr, 5);
    const std::vector<ClauseId> rewritten{0, 2};
    IndexSet dropped_col_idxs(6);
    dropped_col_idxs.set(0);
    dropped_col_idxs.set(3);
    selector.update_with_substitution(rewritten, dropped_col_idxs, trail);
    CHECK(selector.next() == std::nullopt);
    CHECK(database.record(3).watches.watched1 == 2);

    SUBCASE("kept cells stay in their buckets, and rebuilt ones go behind them") {
        learn(selector, trail, {1}, false);
        CHECK(drain(selector) == std::vector<ClauseId>{1, 3, 0});
    }

    SUBCASE("a clause added afterwards takes the cells the inactive one left") {
        add_starting_clause(database, selector, make_clause(repr, 5, {{1, 2}}));
        CHECK(selector.next()->id == 4);
        // Clause 3 watches x1 and x4.
        learn(selector, trail, {2}, false);
        CHECK(drain(selector) == std::vector<ClauseId>{0, 1});
    }

    SUBCASE("a renumbering relabels the cells and keeps each bucket's order") {
        const CompactionTransform transform =
            CompactionTransform::from_dropped_columns(5, dropped_col_idxs);
        trail = Trail(repr, 3);
        renumber(database, selector, transform, trail);
        CHECK(rows_of(database.clause(1)) == std::vector<std::vector<std::size_t>>{{0}, {1}});
        CHECK(selector.next() == std::nullopt);
        CHECK(database.record(3).watches.watched1 == 2);
        // x1 is now column 0, and its bucket is swept as before the renumbering.
        learn(selector, trail, {0}, false);
        CHECK(drain(selector) == std::vector<ClauseId>{1, 3, 0});
    }
}

TEST_CASE("exact watchlist: watches past the first word") {
    constexpr EquationRepr repr = EquationRepr::dense;
    constexpr std::size_t num_vars = 130;
    ClauseDatabase database;
    ExactWatchlistTemplate<DenseGF2Vector> selector(num_vars, database);
    Trail trail(repr, num_vars);
    add_starting_clause(database, selector, make_clause(repr, num_vars, {{100}, {3}}));
    add_starting_clause(database, selector, make_clause(repr, num_vars, {{64}, {127}}));
    add_starting_clause(database, selector, make_clause(repr, num_vars, {{129}, {65}}));
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

TEST_CASE_TEMPLATE("clause database: per-clause records", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    ClauseDatabase database;
    Cyclic selector(database);
    const Trail trail(repr, 4);

    SUBCASE("an input clause is unlearned, with nothing counted and no LBD") {
        add_starting_clause(database, selector, make_clause(repr, 4, {{0}}));
        CHECK_FALSE(database.record(0).learned);
        CHECK_FALSE(database.record(0).used_since_deletion);
        CHECK(database.record(0).unused_passes == 0);
        CHECK_FALSE(database.record(0).lbd_measured);
    }

    SUBCASE("a learned clause takes its LBD") {
        add_learned_clause(database, selector, make_clause(repr, 4, {{0}}), trail,
                             AssertingClauseInfo{.lbd = 3, .removed_levels = 0});
        CHECK(database.record(0).learned);
        CHECK(database.record(0).lbd_measured);
        CHECK(database.record(0).lbd == 3.0);
        CHECK(database.record(0).unused_passes == 0);
    }

    SUBCASE("a width-2 clause counts when it enters, by narrowing or as a new clause at an id") {
        add_starting_clause(database, selector, make_clause(repr, 4, {{0}, {1}}));
        add_starting_clause(database, selector, make_clause(repr, 4, {{0}, {1}, {2}}));
        CHECK(database.num_clauses_reaching_width_2() == 1);
        // Rewritten at its width, the clause is not new.
        database.replace_clause(0, make_clause(repr, 4, {{0}, {2}}));
        CHECK(database.num_clauses_reaching_width_2() == 1);
        database.replace_clause(1, make_clause(repr, 4, {{1}, {2}}));
        CHECK(database.num_clauses_reaching_width_2() == 2);
        database.replace_with_new_clause(0, make_clause(repr, 4, {{0}, {3}}));
        CHECK(database.num_clauses_reaching_width_2() == 3);
    }

    SUBCASE("a clause watches its first rows when it enters, and forgets what satisfied it") {
        add_starting_clause(database, selector, make_clause(repr, 4, {{0}}));
        add_starting_clause(database, selector, make_clause(repr, 4, {{0}, {1}, {2}}));
        CHECK(database.record(0).watches.watched0 == 0);
        CHECK(database.record(0).watches.watched1 == 0);
        CHECK(database.record(1).watches.watched1 == 1);

        ClauseRecord& record = database.record(1);
        record.watches = {.watched0 = 2, .watched1 = 1, .next_watch_search_start = 2};
        record.satisfied_level = 1;
        record.satisfied_stamp = 3;
        database.replace_clause(1, make_clause(repr, 4, {{0}, {3}}));
        CHECK(record.watches.watched0 == 0);
        CHECK(record.watches.watched1 == 1);
        CHECK(record.watches.next_watch_search_start == 0);
        CHECK(record.satisfied_stamp == 0);
    }

    SUBCASE("the active ids are walked in ascending order") {
        const auto active_ids = [&database] {
            std::vector<ClauseId> ids;
            for (const ClauseId id : database.active_clause_ids()) {
                ids.push_back(id);
            }
            return ids;
        };
        for (std::size_t var = 0; var < 4; ++var) {
            add_starting_clause(database, selector, make_clause(repr, 4, {{var}}));
        }
        CHECK(active_ids() == std::vector<ClauseId>{0, 1, 2, 3});
        deactivate_clause(database, selector, 1);
        CHECK(active_ids() == std::vector<ClauseId>{0, 2, 3});
        deactivate_clause(database, selector, 3);
        CHECK(active_ids() == std::vector<ClauseId>{0, 2});
        deactivate_clause(database, selector, 0);
        CHECK(active_ids() == std::vector<ClauseId>{2});
        CHECK(database.active_clause_ids().contains(2));
        CHECK_FALSE(database.active_clause_ids().contains(0));
        deactivate_clause(database, selector, 2);
        CHECK(active_ids().empty());
    }

    SUBCASE("the clauses carrying a column are found whatever replaced or retired them") {
        const auto carrying = [&database](std::initializer_list<std::size_t> col_idxs) {
            IndexSet set(5);
            for (const std::size_t col_idx : col_idxs) {
                set.set(col_idx);
            }
            return database.clause_ids_carrying(set);
        };
        add_starting_clause(database, selector, make_clause(repr, 4, {{0, 1}, {2}}));
        add_starting_clause(database, selector, make_clause(repr, 4, {{1, 3}}));
        add_starting_clause(database, selector, make_clause(repr, 4, {{3}}));
        CHECK(carrying({1}) == std::vector<ClauseId>{0, 1});
        CHECK(carrying({0, 3}) == std::vector<ClauseId>{0, 1, 2});
        CHECK(carrying({}).empty());

        // Dropped, carried again, and dropped: one entry goes stale and comes back.
        database.replace_clause(0, make_clause(repr, 4, {{2}}));
        CHECK(carrying({1}) == std::vector<ClauseId>{1});
        database.replace_clause(0, make_clause(repr, 4, {{1, 2}}));
        CHECK(carrying({1, 2}) == std::vector<ClauseId>{0, 1});
        deactivate_clause(database, selector, 1);
        CHECK(carrying({1, 3}) == std::vector<ClauseId>{0, 2});

        // A clause spanning `0 = 1` carries no variable and is found all the same.
        add_starting_clause(database, selector, make_clause(repr, 4, {{4}}));
        CHECK(carrying({}) == std::vector<ClauseId>{3});
        CHECK(carrying({3}) == std::vector<ClauseId>{2, 3});

        IndexSet dropped(5);
        dropped.set(0);
        renumber(database, selector, CompactionTransform::from_dropped_columns(4, dropped),
                 Trail(repr, 3));
        // The entries move with their columns: old x1, x2, x3 are x0, x1, x2.
        const auto carrying_renumbered = [&database](std::size_t col_idx) {
            IndexSet set(4);
            set.set(col_idx);
            return database.clause_ids_carrying(set);
        };
        CHECK(carrying_renumbered(0) == std::vector<ClauseId>{0, 3});
        CHECK(carrying_renumbered(1) == std::vector<ClauseId>{0, 3});
        CHECK(carrying_renumbered(2) == std::vector<ClauseId>{2, 3});
    }

    SUBCASE("a clause replaced or retired before it was ever looked up is found by its rows") {
        add_starting_clause(database, selector, make_clause(repr, 4, {{0, 1}}));
        add_starting_clause(database, selector, make_clause(repr, 4, {{2}}));
        add_starting_clause(database, selector, make_clause(repr, 4, {{2, 3}}));
        database.replace_clause(0, make_clause(repr, 4, {{1, 3}}));
        deactivate_clause(database, selector, 1);
        const auto carrying = [&database](std::size_t col_idx) {
            IndexSet set(5);
            set.set(col_idx);
            return database.clause_ids_carrying(set);
        };
        CHECK(carrying(0).empty());
        CHECK(carrying(1) == std::vector<ClauseId>{0});
        CHECK(carrying(2) == std::vector<ClauseId>{2});
        CHECK(carrying(3) == std::vector<ClauseId>{0, 2});

        // Entered by now, so a replacement is entered under its new columns only.
        database.replace_clause(0, make_clause(repr, 4, {{0, 3}}));
        CHECK(carrying(0) == std::vector<ClauseId>{0});
        CHECK(carrying(1).empty());
        CHECK(carrying(3) == std::vector<ClauseId>{0, 2});
    }

    SUBCASE("metadata is id-indexed, so compaction leaves it alone") {
        add_starting_clause(database, selector, make_clause(repr, 4, {{0}}));
        add_learned_clause(database, selector, make_clause(repr, 4, {{1}}), trail,
                             AssertingClauseInfo{.lbd = 5, .removed_levels = 0});
        database.record(1).used_since_deletion = true;
        database.record(1).unused_passes = 3;
        deactivate_clause(database, selector, 0);

        selector.update_with_substitution({}, IndexSet(), trail);

        CHECK_FALSE(database.record(0).learned);
        CHECK(database.record(1).learned);
        CHECK(database.record(1).lbd == 5.0);
        CHECK(database.record(1).used_since_deletion);
        CHECK(database.record(1).unused_passes == 3);
    }
}

// Random additions, replacements, retirements, substitutions and renumberings,
// with every lookup compared against a scan of the active clauses.
TEST_CASE_TEMPLATE("clause database: an LBD is the average of the measurements at uses as a reason",
                   Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    ClauseDatabase database;
    Cyclic selector(database);
    const Trail trail(repr, 4);
    add_starting_clause(database, selector, make_clause(repr, 4, {{0}}));
    add_learned_clause(database, selector, make_clause(repr, 4, {{1}}), trail,
                         AssertingClauseInfo{.lbd = 6, .removed_levels = 0});
    ClauseRecord& starting = database.record(0);
    ClauseRecord& learned = database.record(1);

    SUBCASE("a starting clause's first measurement sets its LBD, and later ones average in") {
        starting.add_lbd_measurement(5, 2);
        CHECK(starting.lbd_measured);
        CHECK(starting.lbd == 5.0);
        starting.add_lbd_measurement(3, 2);
        CHECK(starting.lbd == 4.0);
        CHECK_FALSE(starting.learned);
        CHECK_FALSE(starting.used_since_deletion);
    }

    SUBCASE("a learned clause's LBD falls toward a better measurement and rises toward a worse") {
        learned.add_lbd_measurement(2, 2);
        CHECK(learned.lbd == 4.0);
        learned.add_lbd_measurement(2, 2);
        CHECK(learned.lbd == 3.0);
        learned.add_lbd_measurement(9, 2);
        CHECK(learned.lbd == 6.0);
        learned.add_lbd_measurement(9, 2);
        CHECK(learned.lbd == 7.5);
    }

    SUBCASE("the measurement's share is the window's reciprocal") {
        learned.add_lbd_measurement(2, 4);
        CHECK(learned.lbd == 5.0);
        learned.add_lbd_measurement(9, 1);
        CHECK(learned.lbd == 9.0);
        starting.add_lbd_measurement(7, 4);
        CHECK(starting.lbd == 7.0);
    }
}

TEST_CASE_TEMPLATE("clause database: equivalent clauses keep the lower measured LBD", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    ClauseDatabase database;
    Cyclic selector(database);
    const Trail trail(repr, 4);
    add_starting_clause(database, selector, make_clause(repr, 4, {{0}}));
    add_learned_clause(database, selector, make_clause(repr, 4, {{1}}), trail,
                         AssertingClauseInfo{.lbd = 6, .removed_levels = 0});
    ClauseRecord& starting = database.record(0);
    ClauseRecord& learned = database.record(1);

    SUBCASE("both measured: the lower, whichever is learned") {
        learned.take_lower_lbd(4.0);
        CHECK(learned.lbd == 4.0);
        learned.take_lower_lbd(5.0);
        CHECK(learned.lbd == 4.0);
        starting.add_lbd_measurement(3, 2);
        starting.take_lower_lbd(learned.lbd);
        CHECK(starting.lbd == 3.0);
    }

    SUBCASE("an unmeasured clause takes the other's LBD") {
        starting.take_lower_lbd(learned.lbd);
        CHECK(starting.lbd_measured);
        CHECK(starting.lbd == 6.0);
    }

    SUBCASE("an unmeasured other leaves the LBD as it is") {
        learned.take_lower_lbd(std::nullopt);
        CHECK(learned.lbd == 6.0);
        starting.take_lower_lbd(std::nullopt);
        CHECK_FALSE(starting.lbd_measured);
    }
}

TEST_CASE_TEMPLATE("clause database: the column-to-clause entries against a scan", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        TestRandom random(seed);
        std::size_t num_vars = 140;
        ClauseDatabase database;
        Cyclic selector(database);
        IndexSet dropped(num_vars + 1);

        // A clause of one to three rows over the variables not in `avoided`.
        const auto random_clause = [&](const IndexSet& avoided) {
            Clause clause(repr, num_vars);
            while (clause.rows() == 0) {
                for (std::size_t row = random.next(3) + 1; row > 0; --row) {
                    std::vector<std::size_t> bits;
                    for (std::size_t bit = random.next(6); bit > 0; --bit) {
                        const std::size_t var = random.next(num_vars);
                        if (!avoided.test(var) && std::ranges::find(bits, var) == bits.end()) {
                            bits.push_back(var);
                        }
                    }
                    if (random.chance(4)) {
                        bits.push_back(num_vars);
                    }
                    if (!bits.empty()) {
                        clause.add_row(Equation::from_bits(repr, num_vars, bits));
                    }
                }
            }
            return clause;
        };
        const auto retire = [&](ClauseId id) {
            deactivate_clause(database, selector, id);
            database.replace_clause(
                id, inactive_clause_placeholder(repr, Equation::eq_false(repr, num_vars)));
        };
        const auto random_active_id = [&]() -> std::optional<ClauseId> {
            std::vector<ClauseId> ids;
            for (const ClauseId id : database.active_clause_ids()) {
                ids.push_back(id);
            }
            if (ids.empty()) {
                return std::nullopt;
            }
            return ids[random.next(ids.size())];
        };
        const auto check_lookup = [&](const IndexSet& col_idxs) {
            std::vector<ClauseId> scanned;
            for (const ClauseId id : database.active_clause_ids()) {
                const Clause& clause = database.clause(id);
                if (clause.pivot_col_idxs().test(num_vars) ||
                    clause.carried_col_idxs().intersects(col_idxs)) {
                    scanned.push_back(id);
                }
            }
            std::vector<ClauseId> found = database.clause_ids_carrying(col_idxs);
            CHECK(found == scanned);
            return found;
        };

        for (std::size_t step = 0; step < 600 && num_vars > 8; ++step) {
            switch (random.next(8)) {
                case 0:
                case 1:
                case 2:
                    if (random.chance(2)) {
                        add_starting_clause(database, selector, random_clause(dropped));
                    } else {
                        add_learned_clause(database, selector, random_clause(dropped),
                                             Trail(repr, num_vars),
                                             AssertingClauseInfo{.lbd = 2, .removed_levels = 0});
                    }
                    break;
                case 3:
                    if (const std::optional<ClauseId> id = random_active_id()) {
                        database.replace_clause(*id, random_clause(dropped));
                    }
                    break;
                case 4:
                    if (const std::optional<ClauseId> id = random_active_id()) {
                        retire(*id);
                    }
                    break;
                case 5: {
                    IndexSet col_idxs(num_vars + 1);
                    for (std::size_t col = random.next(3); col > 0; --col) {
                        col_idxs.set(random.next(num_vars));
                    }
                    check_lookup(col_idxs);
                    break;
                }
                case 6: {
                    // A substitution: the clauses carrying a newly dropped variable
                    // are rewritten without it or retired.
                    IndexSet newly_dropped(num_vars + 1);
                    for (std::size_t col = random.next(3) + 1; col > 0; --col) {
                        const std::size_t var = random.next(num_vars);
                        if (!dropped.test(var)) {
                            newly_dropped.set(var);
                        }
                    }
                    const std::vector<ClauseId> found = check_lookup(newly_dropped);
                    dropped.or_with(newly_dropped);
                    for (const ClauseId id : found) {
                        if (random.chance(3)) {
                            retire(id);
                        } else {
                            database.replace_clause(id, random_clause(dropped));
                        }
                    }
                    CHECK(check_lookup(newly_dropped).size() <= found.size());
                    break;
                }
                default:
                    if (random.chance(4) && dropped.count() != 0) {
                        // Clauses spanning `0 = 1` may still carry an older dropped variable.
                        for (const ClauseId id : check_lookup(dropped)) {
                            database.replace_clause(id, random_clause(dropped));
                        }
                        const CompactionTransform transform =
                            CompactionTransform::from_dropped_columns(num_vars, dropped);
                        num_vars = transform.new_num_vars();
                        renumber(database, selector, transform, Trail(repr, num_vars));
                        dropped = IndexSet(num_vars + 1);
                    }
                    break;
            }
        }
        IndexSet all(num_vars + 1);
        for (std::size_t var = 0; var < num_vars; ++var) {
            all.set(var);
        }
        check_lookup(all);
    }
}

} // namespace xorcle
