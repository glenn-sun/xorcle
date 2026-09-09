#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "core/clause_id.h"
#include "core/solver.h"
#include "core/trail.h"
#include "heuristics/compaction/compaction.h"
#include "heuristics/decisions/clause_activity.h"
#include "heuristics/decisions/csids.h"
#include "heuristics/deletion/deletion.h"
#include "heuristics/restarts/restart_policy.h"
#include "heuristics/selection/clause_selector.h"
#include "heuristics/selection/cyclic.h"
#include "heuristics_test_support.h"
#include "io/dimacs.h"
#include "util/random.h"

namespace xorcle {

namespace {

constexpr Seed default_seed = 0;

/// @brief Activity read from a table rather than from a decision procedure.
class FixedActivity final : public ClauseActivitySource {
public:
    explicit FixedActivity(std::unordered_map<ClauseId, double> scores)
        : scores_(std::move(scores)) {}

    double clause_activity(ClauseId id) const override {
        const auto found = scores_.find(id);
        return found == scores_.end() ? 0.0 : found->second;
    }

private:
    std::unordered_map<ClauseId, double> scores_;
};

/// @brief A selector holding one unlearned clause under id 0 and then one
/// learned clause per listed LBD, each a distinct unit row.
///
/// The tier cutoffs are left at their defaults, so the caller names the tier it
/// wants by the LBD it passes.
std::unique_ptr<Cyclic> make_selector(EquationRepr repr, std::size_t num_vars,
                                      const std::vector<std::size_t>& learned_lbds) {
    auto selector = std::make_unique<Cyclic>(num_vars);
    const Trail trail(repr, num_vars);
    selector->add_starting_clause(make_clause(repr, num_vars, {{0}}));
    for (std::size_t index = 0; index < learned_lbds.size(); ++index) {
        Clause clause(repr, num_vars);
        clause.add_row(make_equation_with(repr, num_vars, {index + 1}));
        selector->add_asserting_clause(
            std::move(clause), trail,
            AssertingClauseInfo{.lbd = learned_lbds[index], .removed_levels = 0});
    }
    return selector;
}

/// @brief A trail whose rows name `reasons` in order, one variable each.
Trail make_trail_with_reasons(EquationRepr repr, std::size_t num_vars,
                              const std::vector<ClauseId>& reasons) {
    Trail trail(repr, num_vars);
    for (std::size_t index = 0; index < reasons.size(); ++index) {
        trail.learn(make_equation_with(repr, num_vars, {index}), reasons[index]);
    }
    return trail;
}

/// @brief A pass over `conflict_interval` conflicts, demoting a `tier2` clause
/// once it has gone `demote_interval` conflicts unused.
DeletionPlan plan_for(const ClauseSelector& selector, const Trail& trail, DeletionKey key,
                      const ClauseActivitySource* activity = nullptr,
                      std::size_t conflict_interval = 100,
                      std::size_t demote_interval = 200) {
    return build_deletion_plan(DeletionState{.selector = selector,
                                             .trail = trail,
                                             .conflict_interval = conflict_interval,
                                             .demote_interval = demote_interval,
                                             .key = key,
                                             .activity = activity});
}

bool contains(const std::vector<ClauseId>& ids, ClauseId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// --- Solver-level runs -----------------------------------------------------

/// @brief All sixteen sign patterns over four variables: UNSAT, and every
/// assignment is ruled out by exactly one clause, so the search must decide.
constexpr std::string_view search_unsat_instance = "p cnf 4 16\n"
                                                   "1 2 3 4 0\n"
                                                   "1 2 3 -4 0\n"
                                                   "1 2 -3 4 0\n"
                                                   "1 2 -3 -4 0\n"
                                                   "1 -2 3 4 0\n"
                                                   "1 -2 3 -4 0\n"
                                                   "1 -2 -3 4 0\n"
                                                   "1 -2 -3 -4 0\n"
                                                   "-1 2 3 4 0\n"
                                                   "-1 2 3 -4 0\n"
                                                   "-1 2 -3 4 0\n"
                                                   "-1 2 -3 -4 0\n"
                                                   "-1 -2 3 4 0\n"
                                                   "-1 -2 3 -4 0\n"
                                                   "-1 -2 -3 4 0\n"
                                                   "-1 -2 -3 -4 0\n";

/// @brief Solve `search_unsat_instance` once, deleting on the given cadence.
///
/// The default cutoffs put every learned clause in `local`, so a run of any
/// length evicts.
SolverCounters solve_deleting(EquationRepr repr, std::size_t conflict_interval,
                              ClauseTierConfig tiers = {.core_lbd = 0, .tier2_lbd = 0},
                              std::size_t demote_interval = 200) {
    ParsedDimacs parsed = parse_dimacs_string(search_unsat_instance, repr);
    const DeletionConfig deletion{.enabled = true,
                                  .conflict_interval = conflict_interval,
                                  .demote_interval = demote_interval,
                                  .tiers = tiers,
                                  .key = DeletionKey::lbd,
                                  .activity = nullptr};

    auto selector = std::make_unique<Cyclic>(parsed.num_vars);
    selector->set_tier_config(deletion.tiers);
    Solver solver(repr, parsed.num_vars, std::move(selector),
                  std::make_unique<Csids>(repr, parsed.num_vars, default_seed),
                  std::make_unique<NoRestarts>(), nullptr, CompactionConfig{}, ProgressConfig{},
                  true, deletion);
    for (Clause& clause : parsed.clauses) {
        solver.add_starting_clause(std::move(clause));
    }
    const Solution solution = solver.solve();
    REQUIRE(solution.result == SolveResult::unsat);
    return solver.counters();
}

} // namespace

TEST_CASE_TEMPLATE("deletion: a locked clause is neither evicted nor counted", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    // Four local clauses under ids 1..4, the first of them the worst by LBD.
    const std::unique_ptr<Cyclic> selector = make_selector(repr, 8, {12, 11, 10, 9});
    REQUIRE(selector->clause_tier(1) == ClauseTier::local);

    SUBCASE("the reason a trail row names survives its own key") {
        const Trail trail = make_trail_with_reasons(repr, 8, {ClauseId{1}});
        const DeletionPlan plan = plan_for(*selector, trail, DeletionKey::lbd);

        // Of the three unlocked candidates, only the worst goes.
        CHECK(plan.evicted == std::vector<ClauseId>{2});
        CHECK_FALSE(contains(plan.evicted, ClauseId{1}));
    }

    SUBCASE("without the lock the same clause is the first to go") {
        const Trail trail(repr, 8);
        const DeletionPlan plan = plan_for(*selector, trail, DeletionKey::lbd);
        CHECK(plan.evicted == std::vector<ClauseId>{1, 2});
    }

    SUBCASE("a locked tier2 clause does not age") {
        const std::unique_ptr<Cyclic> tier2 = make_selector(repr, 8, {5, 5});
        REQUIRE(tier2->clause_tier(1) == ClauseTier::tier2);
        const Trail trail = make_trail_with_reasons(repr, 8, {ClauseId{1}});

        const DeletionPlan plan = plan_for(*tier2, trail, DeletionKey::lbd);
        CHECK(plan.unused_conflicts[1] == 0);
        CHECK(plan.unused_conflicts[2] == 100);
        CHECK(plan.demoted.empty());
    }
}

TEST_CASE_TEMPLATE("deletion: tier2 demotion", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const std::unique_ptr<Cyclic> selector = make_selector(repr, 8, {5});
    const Trail trail(repr, 8);
    REQUIRE(selector->clause_tier(1) == ClauseTier::tier2);

    SUBCASE("the count accrues a pass at a time, and starts over at the demotion") {
        const DeletionPlan first = plan_for(*selector, trail, DeletionKey::lbd);
        CHECK(first.demoted.empty());
        CHECK(first.unused_conflicts[1] == 100);

        selector->set_clause_unused_conflicts(1, first.unused_conflicts[1]);
        const DeletionPlan second = plan_for(*selector, trail, DeletionKey::lbd);
        CHECK(second.demoted == std::vector<ClauseId>{1});
        CHECK(second.unused_conflicts[1] == 0);
    }

    SUBCASE("a pass wider than the demote interval demotes at the first one") {
        CHECK(plan_for(*selector, trail, DeletionKey::lbd, nullptr, 200, 200).demoted ==
              std::vector<ClauseId>{1});
        CHECK(plan_for(*selector, trail, DeletionKey::lbd, nullptr, 100, 1000).demoted.empty());
    }

    SUBCASE("a use resets the count") {
        selector->set_clause_unused_conflicts(1, 100);
        selector->set_clause_used_since_deletion(1, true);

        const DeletionPlan plan = plan_for(*selector, trail, DeletionKey::lbd);
        CHECK(plan.demoted.empty());
        CHECK(plan.unused_conflicts[1] == 0);
    }

    SUBCASE("a clause demoted here is not also evicted here") {
        selector->set_clause_unused_conflicts(1, 100);
        const DeletionPlan plan = plan_for(*selector, trail, DeletionKey::lbd);
        REQUIRE(plan.demoted == std::vector<ClauseId>{1});
        CHECK(plan.evicted.empty());
    }
}

TEST_CASE_TEMPLATE("deletion: what the keys order", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Trail trail(repr, 8);

    SUBCASE("lbd evicts the highest LBD, and the older id of a tie") {
        const std::unique_ptr<Cyclic> selector = make_selector(repr, 8, {9, 12, 12, 10});
        const DeletionPlan plan = plan_for(*selector, trail, DeletionKey::lbd);
        // Ordered 2, 3, 4, 1; the worse half is the first two, reported ascending.
        CHECK(plan.evicted == std::vector<ClauseId>{2, 3});
    }

    SUBCASE("activity evicts the lowest activity, and the older id of a tie") {
        const std::unique_ptr<Cyclic> selector = make_selector(repr, 8, {9, 9, 9, 9});
        const FixedActivity activity({{1, 4.0}, {2, 1.0}, {3, 1.0}, {4, 8.0}});
        const DeletionPlan plan = plan_for(*selector, trail, DeletionKey::activity, &activity);
        CHECK(plan.evicted == std::vector<ClauseId>{2, 3});
    }

    SUBCASE("the two keys disagree, so each is really being read") {
        const std::unique_ptr<Cyclic> selector = make_selector(repr, 8, {12, 11});
        const FixedActivity activity({{1, 9.0}, {2, 1.0}});
        CHECK(plan_for(*selector, trail, DeletionKey::lbd).evicted ==
              std::vector<ClauseId>{1});
        CHECK(plan_for(*selector, trail, DeletionKey::activity, &activity).evicted ==
              std::vector<ClauseId>{2});
    }
}

TEST_CASE_TEMPLATE("deletion: what is never a candidate", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Trail trail(repr, 8);

    SUBCASE("an input clause, however the cutoffs are set") {
        auto selector = std::make_unique<Cyclic>(8);
        selector->set_tier_config(ClauseTierConfig{.core_lbd = 0, .tier2_lbd = 0});
        for (std::size_t index = 0; index < 4; ++index) {
            selector->add_starting_clause(make_clause(repr, 8, {{index}}));
        }
        REQUIRE(selector->clause_tier(0) == ClauseTier::local);

        const DeletionPlan plan = plan_for(*selector, trail, DeletionKey::lbd);
        CHECK(plan.evicted.empty());
    }

    SUBCASE("a core clause") {
        const std::unique_ptr<Cyclic> selector = make_selector(repr, 8, {1, 2, 3, 3});
        const DeletionPlan plan = plan_for(*selector, trail, DeletionKey::lbd);
        CHECK(plan.evicted.empty());
        CHECK(plan.demoted.empty());
    }

    SUBCASE("a clause already tombstoned") {
        const std::unique_ptr<Cyclic> selector = make_selector(repr, 8, {9, 9, 9, 9});
        selector->set_clause_active(1, false);
        selector->set_clause_active(2, false);

        // Two candidates are left, so exactly one goes.
        const DeletionPlan plan = plan_for(*selector, trail, DeletionKey::lbd);
        CHECK(plan.evicted == std::vector<ClauseId>{3});
    }
}

TEST_CASE_TEMPLATE("deletion: the tombstone is compaction's placeholder", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Equation false_row = Equation::eq_false(repr, 4);
    const Clause placeholder = inactive_clause_placeholder(repr, false_row);

    CHECK(placeholder.num_vars() == 4);
    CHECK(placeholder.rows() == 1);
    CHECK(placeholder.row(0) == false_row);
    CHECK(placeholder.watched0 == 0);
    CHECK(placeholder.watched1 == 0);
    CHECK(placeholder.next_watch_search_start == 0);

    // The shape a compaction plan writes for a clause the root trail satisfies.
    Trail trail(repr, 5);
    // `x1 = 1` satisfies the clause below, which is `[x1 = 1] v [x2 = 1]`.
    trail.learn(make_equation_with(repr, 5, {0, 5}), ClauseId{0});
    const std::vector<Clause> clauses{make_clause(repr, 5, {{0}, {1}})};
    const std::vector<std::uint8_t> active{1};
    const std::vector<std::size_t> var_map{0, 1, 2, 3, 4};
    const CompactionState state{trail,  clauses, active, var_map, 5, false, 0, 0, 16};
    const CompactionTransform transform = build_compaction_transform(trail, 5);
    const CompactionPlan compacted = build_compaction_plan(state, transform);

    REQUIRE(compacted.clause_is_active[0] == 0);
    const Clause& tombstone = compacted.rewritten_clauses[0];
    CHECK(tombstone.rows() == 1);
    CHECK(tombstone.row(0) == Equation::eq_false(repr, tombstone.num_vars()));
    CHECK(tombstone.watched0 == 0);
    CHECK(tombstone.watched1 == 0);
    CHECK(tombstone.next_watch_search_start == 0);
}

TEST_CASE_TEMPLATE("deletion: a run deletes, and deletes the same way twice", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const SolverCounters first = solve_deleting(repr, 1);
    const SolverCounters second = solve_deleting(repr, 1);

    CHECK(first.num_deletion_passes != 0);
    CHECK(first.num_deleted_clauses != 0);

    CHECK(first.num_conflicts == second.num_conflicts);
    CHECK(first.num_propagations == second.num_propagations);
    CHECK(first.num_deletion_passes == second.num_deletion_passes);
    CHECK(first.num_deleted_clauses == second.num_deleted_clauses);
    CHECK(first.num_demoted_clauses == second.num_demoted_clauses);
    CHECK(first.num_promoted_clauses == second.num_promoted_clauses);
}

TEST_CASE_TEMPLATE("deletion: a reason resolved at a better LBD is promoted", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    // Cutoffs low enough that a clause can be learned below them and still be
    // resolved against later at an LBD that crosses one.
    const SolverCounters counters =
        solve_deleting(repr, 1, ClauseTierConfig{.core_lbd = 1, .tier2_lbd = 2});
    CHECK(counters.num_promoted_clauses != 0);

    // With every learned clause already in `core` only starting clauses move up,
    // out of the `local` they enter at.
    const SolverCounters all_core =
        solve_deleting(repr, 1, ClauseTierConfig{.core_lbd = 64, .tier2_lbd = 64});
    CHECK(all_core.num_deleted_clauses == 0);
}

TEST_CASE_TEMPLATE("deletion: a shorter demote interval demotes more", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    // Cutoffs that leave a tier2 to demote out of.
    constexpr ClauseTierConfig tiers{.core_lbd = 1, .tier2_lbd = 2};

    CHECK(solve_deleting(repr, 1, tiers, 1).num_demoted_clauses != 0);
    CHECK(solve_deleting(repr, 1, tiers, 1000).num_demoted_clauses == 0);
}

TEST_CASE_TEMPLATE("deletion: a wider interval deletes less often", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const SolverCounters often = solve_deleting(repr, 1);
    const SolverCounters seldom = solve_deleting(repr, 1000);

    CHECK(often.num_deletion_passes > seldom.num_deletion_passes);
    CHECK(seldom.num_deletion_passes == 0);
    CHECK(seldom.num_deleted_clauses == 0);
}

} // namespace xorcle
