#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
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
        trail.learn(make_equation_with(repr, num_vars, {index}), reasons[index], trail.level());
    }
    return trail;
}

DeletionPlan plan_for(const ClauseSelector& selector, const Trail& trail, DeletionKey key,
                      const ClauseActivitySource* activity = nullptr,
                      double eviction_rate = DeletionConfig{}.eviction_rate) {
    return build_deletion_plan(DeletionState{.selector = selector,
                                             .trail = trail,
                                             .wait = DeletionConfig{}.wait,
                                             .eviction_rate = eviction_rate,
                                             .key = key,
                                             .activity = activity});
}

bool contains(const std::vector<ClauseId>& ids, ClauseId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// --- Solver-level runs -----------------------------------------------------

/// @brief A cadence coefficient whose gap between passes is one conflict for the first 22500
/// with no cadence offset.
constexpr double every_conflict = 0.01;

/// @brief Solve the sign patterns over four variables once, deleting on the given cadence.
///
/// The default `wait` of zero allows only a clause of LBD 1 an unused pass, so a run
/// of any length deletes.
SolverCounters solve_deleting(EquationRepr repr, double cadence_coefficient, double wait = 0.0) {
    ParsedDimacs parsed = parse_dimacs_string(sign_pattern_instance(4), repr);
    const DeletionConfig deletion{.enabled = true,
                                  .cadence_coefficient = cadence_coefficient,
                                  .cadence_offset = 0,
                                  .wait = wait,
                                  .key = DeletionKey::lbd};

    Solver solver(repr, parsed.num_vars, parsed.used_vars,
                  std::make_unique<Cyclic>(parsed.num_vars),
                  std::make_unique<Csids>(repr, parsed.num_vars, default_seed),
                  std::make_unique<NoRestarts>(), nullptr, CompactionConfig{}, ProgressConfig{},
                  true, deletion, false, false, PropagationConfig{},
                  ImplicationGraphConfig{.enabled = false});
    for (Clause& clause : parsed.clauses) {
        solver.add_starting_clause(std::move(clause));
    }
    const Solution solution = solver.solve();
    REQUIRE(solution.result == SolveResult::unsat);
    return solver.counters();
}

} // namespace

TEST_CASE("deletion: the unused passes an LBD allows") {
    CHECK(allowed_unused_passes(6.0, 6.0) == 1);
    CHECK(allowed_unused_passes(5.0, 6.0) == 2);
    CHECK(allowed_unused_passes(1.0, 6.0) == 32);
    CHECK(allowed_unused_passes(7.0, 7.0) == 1);

    SUBCASE("a fractional LBD rounds to the nearest pass") {
        CHECK(allowed_unused_passes(2.5, 6.0) == 11); // 11.31
        CHECK(allowed_unused_passes(4.5, 6.0) == 3);  // 2.83
        CHECK(allowed_unused_passes(5.5, 6.0) == 1);  // 1.41
    }

    SUBCASE("half a pass rounds up to one, and less to none") {
        CHECK(allowed_unused_passes(7.0, 6.0) == 1);
        CHECK(allowed_unused_passes(7.25, 6.0) == 0);
        CHECK(allowed_unused_passes(8.0, 6.0) == 0);
    }
}

TEST_CASE("deletion: the conflicts between passes") {
    SUBCASE("with no offset, the coefficient times the square root of the conflicts") {
        const DeletionConfig by_sqrt{.cadence_coefficient = 0.5, .cadence_offset = 0};
        CHECK(conflicts_between_deletion_passes(by_sqrt, 10000) == 50);
        CHECK(conflicts_between_deletion_passes(by_sqrt, 30) == 3); // 2.74
        CHECK(conflicts_between_deletion_passes(by_sqrt, 0) == 1);
    }

    SUBCASE("the offset is added to the conflicts under the square root") {
        const DeletionConfig offset{.cadence_coefficient = 0.5, .cadence_offset = 400};
        CHECK(conflicts_between_deletion_passes(offset, 0) == 10);
        CHECK(conflicts_between_deletion_passes(offset, 9600) == 50);
    }

    SUBCASE("the defaults") {
        const DeletionConfig defaults;
        CHECK(conflicts_between_deletion_passes(defaults, 0) == 18);         // 17.68
        CHECK(conflicts_between_deletion_passes(defaults, 100) == 18);       // 17.85
        CHECK(conflicts_between_deletion_passes(defaults, 995000) == 250);
        CHECK(conflicts_between_deletion_passes(defaults, 1000000) == 251);  // 250.62
    }

    SUBCASE("the default coefficient with no offset") {
        const DeletionConfig no_offset{.cadence_offset = 0};
        CHECK(conflicts_between_deletion_passes(no_offset, 0) == 1);
        CHECK(conflicts_between_deletion_passes(no_offset, 100) == 3);
        CHECK(conflicts_between_deletion_passes(no_offset, 1000000) == 250);
    }
}

TEST_CASE_TEMPLATE("deletion: a use as a reason averages the measured LBD in", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const std::unique_ptr<Cyclic> selector = make_selector(repr, 8, {6});
    REQUIRE(selector->clause_lbd(1) == 6.0);

    SUBCASE("the LBD falls toward a better measurement and rises toward a worse one") {
        update_with_reason_use(*selector, 1, 2, DeletionConfig{});
        CHECK(selector->clause_lbd(1) == 4.0);
        update_with_reason_use(*selector, 1, 2, DeletionConfig{});
        CHECK(selector->clause_lbd(1) == 3.0);
        update_with_reason_use(*selector, 1, 9, DeletionConfig{});
        CHECK(selector->clause_lbd(1) == 6.0);
        update_with_reason_use(*selector, 1, 9, DeletionConfig{});
        CHECK(selector->clause_lbd(1) == 7.5);
    }

    SUBCASE("the measurement's share is the window's reciprocal") {
        update_with_reason_use(*selector, 1, 2, DeletionConfig{.lbd_window = 4});
        CHECK(selector->clause_lbd(1) == 5.0);
        update_with_reason_use(*selector, 1, 9, DeletionConfig{.lbd_window = 1});
        CHECK(selector->clause_lbd(1) == 9.0);
    }

    SUBCASE("the use is recorded for the next pass") {
        update_with_reason_use(*selector, 1, 6, DeletionConfig{});
        CHECK(selector->is_clause_used_since_deletion(1));
    }

    SUBCASE("a starting clause records nothing") {
        update_with_reason_use(*selector, 0, 2, DeletionConfig{});
        CHECK_FALSE(selector->is_clause_used_since_deletion(0));
    }
}

TEST_CASE_TEMPLATE("deletion: a locked clause is neither deleted nor counted", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    // Four candidates under ids 1..4, the first of them the worst by LBD.
    const std::unique_ptr<Cyclic> selector = make_selector(repr, 8, {12, 11, 10, 9});
    REQUIRE(is_deletion_candidate(*selector, 1, 6.0));

    SUBCASE("the reason a trail row names survives its own key") {
        const Trail trail = make_trail_with_reasons(repr, 8, {ClauseId{1}});
        const DeletionPlan plan = plan_for(*selector, trail, DeletionKey::lbd);

        // Of the three unlocked candidates, only the worst goes.
        CHECK(plan.deleted == std::vector<ClauseId>{2});
        CHECK_FALSE(contains(plan.deleted, ClauseId{1}));
    }

    SUBCASE("without the lock the same clause is the first to go") {
        const Trail trail(repr, 8);
        const DeletionPlan plan = plan_for(*selector, trail, DeletionKey::lbd);
        CHECK(plan.deleted == std::vector<ClauseId>{1, 2});
    }

    SUBCASE("a locked clause with passes left does not age") {
        const std::unique_ptr<Cyclic> allowed = make_selector(repr, 8, {5, 5});
        const Trail trail = make_trail_with_reasons(repr, 8, {ClauseId{1}});

        const DeletionPlan plan = plan_for(*allowed, trail, DeletionKey::lbd);
        CHECK(plan.unused_passes[1] == 0);
        CHECK(plan.unused_passes[2] == 1);
        CHECK(plan.out_of_unused_passes.empty());
    }

    SUBCASE("the counts follow the active ids, whatever order the trail names its reasons in") {
        const std::unique_ptr<Cyclic> allowed = make_selector(repr, 8, {5, 5, 5, 5, 5});
        allowed->set_clause_active(2, false);
        const Trail trail = make_trail_with_reasons(repr, 8, {ClauseId{5}, ClauseId{1}});

        // Ids 0, 1, 3, 4, 5: the starting clause, then 1 and 5 locked, 3 and 4 aging.
        const DeletionPlan plan = plan_for(*allowed, trail, DeletionKey::lbd);
        CHECK(plan.unused_passes == std::vector<std::size_t>{0, 0, 1, 1, 0});
        CHECK(plan.out_of_unused_passes.empty());
        CHECK(plan.deleted.empty());
    }
}

TEST_CASE_TEMPLATE("deletion: running out of unused passes", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    // LBD 5 allows two unused passes, LBD 4 four.
    const std::unique_ptr<Cyclic> selector = make_selector(repr, 8, {5, 4});
    const Trail trail(repr, 8);
    REQUIRE_FALSE(is_deletion_candidate(*selector, 1, 6.0));

    SUBCASE("the count accrues a pass at a time, to what the LBD allows") {
        const DeletionPlan first = plan_for(*selector, trail, DeletionKey::lbd);
        CHECK(first.out_of_unused_passes.empty());
        CHECK(first.unused_passes[1] == 1);
        CHECK(first.unused_passes[2] == 1);

        selector->set_clause_unused_passes(1, 1);
        selector->set_clause_unused_passes(2, 1);
        const DeletionPlan second = plan_for(*selector, trail, DeletionKey::lbd);
        CHECK(second.out_of_unused_passes == std::vector<ClauseId>{1});
        CHECK(second.unused_passes[2] == 2);
    }

    SUBCASE("a use resets the count") {
        selector->set_clause_unused_passes(1, 1);
        selector->set_clause_used_since_deletion(1, true);

        const DeletionPlan plan = plan_for(*selector, trail, DeletionKey::lbd);
        CHECK(plan.out_of_unused_passes.empty());
        CHECK(plan.unused_passes[1] == 0);
    }

    SUBCASE("a clause running out here is not also deleted here, but at the next pass") {
        selector->set_clause_unused_passes(1, 1);
        selector->set_clause_unused_passes(2, 3);
        const DeletionPlan plan = plan_for(*selector, trail, DeletionKey::lbd);
        REQUIRE(plan.out_of_unused_passes == std::vector<ClauseId>{1, 2});
        CHECK(plan.deleted.empty());

        selector->set_clause_out_of_unused_passes(1, true);
        selector->set_clause_out_of_unused_passes(2, true);
        CHECK(is_deletion_candidate(*selector, 1, 6.0));
        CHECK(plan_for(*selector, trail, DeletionKey::lbd).deleted == std::vector<ClauseId>{1});
    }

    SUBCASE("a higher wait allows more passes") {
        selector->set_clause_unused_passes(1, 1);
        const DeletionPlan plan = build_deletion_plan(DeletionState{.selector = *selector,
                                                                    .trail = trail,
                                                                    .wait = 7.0,
                                                                    .eviction_rate = 0.5,
                                                                    .key = DeletionKey::lbd,
                                                                    .activity = nullptr});
        CHECK(plan.out_of_unused_passes.empty());
        CHECK(plan.unused_passes[1] == 2);
    }
}

TEST_CASE_TEMPLATE("deletion: a use as a reason and a deletion candidate", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const std::unique_ptr<Cyclic> selector = make_selector(repr, 8, {5, 9});
    const Trail trail(repr, 8);

    SUBCASE("a clause out of unused passes is allowed them afresh") {
        selector->set_clause_unused_passes(1, 2);
        selector->set_clause_out_of_unused_passes(1, true);
        REQUIRE(is_deletion_candidate(*selector, 1, 6.0));

        update_with_reason_use(*selector, 1, 5, DeletionConfig{});
        CHECK_FALSE(is_deletion_candidate(*selector, 1, 6.0));
        CHECK(selector->clause_unused_passes(1) == 0);

        const DeletionPlan plan = plan_for(*selector, trail, DeletionKey::lbd);
        CHECK_FALSE(contains(plan.deleted, ClauseId{1}));
        CHECK(plan.unused_passes[1] == 0);
    }

    SUBCASE("a clause whose LBD allowed none is allowed some once its LBD does") {
        REQUIRE(is_deletion_candidate(*selector, 2, 6.0));
        update_with_reason_use(*selector, 2, 3, DeletionConfig{});
        CHECK(selector->clause_lbd(2) == 6.0);
        CHECK_FALSE(is_deletion_candidate(*selector, 2, 6.0));
    }

    SUBCASE("a use at too high an LBD leaves a candidate one") {
        selector->set_clause_out_of_unused_passes(1, true);
        update_with_reason_use(*selector, 1, 11, DeletionConfig{});
        CHECK(selector->clause_lbd(1) == 8.0);
        CHECK(is_deletion_candidate(*selector, 1, 6.0));
    }

    SUBCASE("a use at too high an LBD makes a candidate of a clause with passes left") {
        selector->set_clause_unused_passes(1, 1);
        update_with_reason_use(*selector, 1, 11, DeletionConfig{});
        REQUIRE(is_deletion_candidate(*selector, 1, 6.0));

        // The use does not protect it: both candidates are ordered, and the worse goes.
        CHECK(plan_for(*selector, trail, DeletionKey::lbd).deleted == std::vector<ClauseId>{2});
    }

    SUBCASE("a use that leaves passes to spare keeps the count for the pass to reset") {
        selector->set_clause_unused_passes(1, 1);
        update_with_reason_use(*selector, 1, 5, DeletionConfig{});
        CHECK(selector->clause_unused_passes(1) == 1);
        CHECK(plan_for(*selector, trail, DeletionKey::lbd).unused_passes[1] == 0);
    }
}

TEST_CASE_TEMPLATE("deletion: what the keys order", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Trail trail(repr, 8);

    SUBCASE("lbd deletes the highest LBD, and the older id of a tie") {
        const std::unique_ptr<Cyclic> selector = make_selector(repr, 8, {9, 12, 12, 10});
        const DeletionPlan plan = plan_for(*selector, trail, DeletionKey::lbd);
        // Ordered 2, 3, 4, 1; the first two are evicted, reported ascending.
        CHECK(plan.deleted == std::vector<ClauseId>{2, 3});
    }

    SUBCASE("lbd reads the averaged LBD") {
        const std::unique_ptr<Cyclic> selector = make_selector(repr, 8, {9, 9});
        selector->set_clause_lbd(2, 9.5);
        CHECK(plan_for(*selector, trail, DeletionKey::lbd).deleted == std::vector<ClauseId>{2});
    }

    SUBCASE("activity deletes the lowest activity, and the older id of a tie") {
        const std::unique_ptr<Cyclic> selector = make_selector(repr, 8, {9, 9, 9, 9});
        const FixedActivity activity({{1, 4.0}, {2, 1.0}, {3, 1.0}, {4, 8.0}});
        const DeletionPlan plan = plan_for(*selector, trail, DeletionKey::activity, &activity);
        CHECK(plan.deleted == std::vector<ClauseId>{2, 3});
    }

    SUBCASE("the two keys disagree, so each is really being read") {
        const std::unique_ptr<Cyclic> selector = make_selector(repr, 8, {12, 11});
        const FixedActivity activity({{1, 9.0}, {2, 1.0}});
        CHECK(plan_for(*selector, trail, DeletionKey::lbd).deleted ==
              std::vector<ClauseId>{1});
        CHECK(plan_for(*selector, trail, DeletionKey::activity, &activity).deleted ==
              std::vector<ClauseId>{2});
    }
}

TEST_CASE_TEMPLATE("deletion: the eviction rate is the fraction of the candidates deleted", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Trail trail(repr, 8);
    const std::unique_ptr<Cyclic> even = make_selector(repr, 8, {12, 11, 10, 9});
    const std::unique_ptr<Cyclic> odd = make_selector(repr, 8, {13, 12, 11, 10, 9});
    const auto evicted = [&](const Cyclic& selector, double eviction_rate) {
        return plan_for(selector, trail, DeletionKey::lbd, nullptr, eviction_rate).deleted;
    };

    CHECK(evicted(*even, 0.0).empty());
    CHECK(evicted(*odd, 0.0).empty());
    CHECK(evicted(*even, 0.5) == std::vector<ClauseId>{1, 2});
    CHECK(evicted(*odd, 0.5) == std::vector<ClauseId>{1, 2});
    CHECK(evicted(*even, 0.75) == std::vector<ClauseId>{1, 2, 3});
    CHECK(evicted(*odd, 0.75) == std::vector<ClauseId>{1, 2, 3});
    CHECK(evicted(*even, 1.0) == std::vector<ClauseId>{1, 2, 3, 4});
    CHECK(evicted(*odd, 1.0) == std::vector<ClauseId>{1, 2, 3, 4, 5});
}

TEST_CASE_TEMPLATE("deletion: what is never a candidate", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Trail trail(repr, 8);

    SUBCASE("an input clause") {
        auto selector = std::make_unique<Cyclic>(8);
        for (std::size_t index = 0; index < 4; ++index) {
            selector->add_starting_clause(make_clause(repr, 8, {{index}}));
        }

        const DeletionPlan plan = plan_for(*selector, trail, DeletionKey::lbd);
        CHECK(plan.deleted.empty());
        CHECK(plan.out_of_unused_passes.empty());
        CHECK(plan.unused_passes[0] == 0);
    }

    SUBCASE("a clause with unused passes left") {
        const std::unique_ptr<Cyclic> selector = make_selector(repr, 8, {1, 2, 3, 3});
        const DeletionPlan plan = plan_for(*selector, trail, DeletionKey::lbd);
        CHECK(plan.deleted.empty());
        CHECK(plan.out_of_unused_passes.empty());
    }

    SUBCASE("a clause already deleted") {
        const std::unique_ptr<Cyclic> selector = make_selector(repr, 8, {9, 9, 9, 9});
        selector->set_clause_active(1, false);
        selector->set_clause_active(2, false);

        // Two candidates are left, so exactly one goes.
        const DeletionPlan plan = plan_for(*selector, trail, DeletionKey::lbd);
        CHECK(plan.deleted == std::vector<ClauseId>{3});
    }
}

TEST_CASE_TEMPLATE("deletion: a deleted clause is compaction's placeholder", Tag, DenseTag,
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
}

TEST_CASE_TEMPLATE("deletion: a run deletes, and deletes the same way twice", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const SolverCounters first = solve_deleting(repr, every_conflict);
    const SolverCounters second = solve_deleting(repr, every_conflict);

    CHECK(first.num_deletion_passes != 0);
    CHECK(first.num_deleted_clauses != 0);

    CHECK(first.num_conflicts == second.num_conflicts);
    CHECK(first.num_propagations == second.num_propagations);
    CHECK(first.num_deletion_passes == second.num_deletion_passes);
    CHECK(first.num_deleted_clauses == second.num_deleted_clauses);
}

TEST_CASE_TEMPLATE("deletion: a run whose learned clauses never run out deletes none", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const SolverCounters counters = solve_deleting(repr, every_conflict, 62.0);
    CHECK(counters.num_deletion_passes != 0);
    CHECK(counters.num_deleted_clauses == 0);
}

TEST_CASE_TEMPLATE("deletion: a longer pass cadence deletes less often", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const SolverCounters often = solve_deleting(repr, every_conflict);
    const SolverCounters seldom = solve_deleting(repr, 1000.0);

    CHECK(often.num_deletion_passes > seldom.num_deletion_passes);
    CHECK(seldom.num_deletion_passes == 0);
    CHECK(seldom.num_deleted_clauses == 0);
}

} // namespace xorcle
