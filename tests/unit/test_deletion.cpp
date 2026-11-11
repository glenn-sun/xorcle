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
#include "heuristics/decisions/csids.h"
#include "heuristics/deletion/deletion.h"
#include "heuristics/restarts/restart_policy.h"
#include "core/clause_database.h"
#include "heuristics/selection/cyclic.h"
#include "heuristics_test_support.h"
#include "io/dimacs.h"
#include "util/random.h"

namespace xorcle {

namespace {

/// @brief A database holding one unlearned clause under id 0 and then one
/// learned clause per listed LBD, each a distinct unit row.
std::unique_ptr<ClauseDatabase> make_database(EquationRepr repr, std::size_t num_vars,
                                              const std::vector<std::size_t>& learned_lbds) {
    auto database = std::make_unique<ClauseDatabase>();
    database->add_starting_clause(make_clause(repr, num_vars, {{0}}));
    for (std::size_t index = 0; index < learned_lbds.size(); ++index) {
        Clause clause(repr, num_vars);
        clause.add_row(make_equation_with(repr, num_vars, {index + 1}));
        database->add_asserting_clause(std::move(clause), learned_lbds[index]);
    }
    return database;
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

/// @brief A CSIDS over `database`'s clauses in which clause `id` was bumped
/// `bumps[id]` times; with no conflict in between, each bump adds 1.
std::unique_ptr<Csids> csids_with_bumps(const ClauseDatabase& database, const Trail& trail,
                                        const std::unordered_map<ClauseId, std::size_t>& bumps) {
    auto csids =
        std::make_unique<Csids>(trail.repr(), trail.num_vars(), default_seed, default_csids_config());
    for (ClauseId id = 0; id < database.num_clauses(); ++id) {
        csids->update_with_new_clause(database.clauses()[id], id);
    }
    for (const auto& [id, count] : bumps) {
        for (std::size_t bump = 0; bump < count; ++bump) {
            csids->update_with_resolved_clause(id);
        }
    }
    return csids;
}

DeletionPlan plan_for(const ClauseDatabase& database, const Trail& trail, DeletionKey key,
                      const Csids* activity = nullptr,
                      double eviction_rate = default_deletion_config().eviction_rate) {
    const std::unique_ptr<Csids> unbumped = csids_with_bumps(database, trail, {});
    DeletionConfig config = default_deletion_config();
    config.eviction_rate = eviction_rate;
    config.key = key;
    return build_deletion_plan(
        DeletionState{.database = database,
                      .trail = trail,
                      .config = config,
                      .activity = activity != nullptr ? *activity : *unbumped});
}

/// @brief What the solver does to a clause's record at a use as a reason, with deletion on.
void use_as_reason(ClauseRecord& record, std::size_t measured_lbd) {
    record.add_lbd_measurement(measured_lbd, default_solver_config().lbd_window);
    update_with_reason_use(record);
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
RunStats solve_deleting(EquationRepr repr, double cadence_coefficient, double wait = 0.0) {
    ParsedDimacs parsed = parse_dimacs_string(sign_pattern_instance(4), repr);
    SolverConfig config = default_solver_config();
    config.otf_subsume = false;
    config.deletion = default_deletion_config();
    config.deletion->cadence_coefficient = cadence_coefficient;
    config.deletion->cadence_offset = 0;
    config.deletion->wait = wait;
    config.deletion->key = DeletionKey::lbd;
    config.implication_graph.reset();
    config.vivify.reset();
    config.bve.reset();

    Solver solver(repr, parsed.num_vars, parsed.used_vars,
                  [](const ClauseDatabase& database) {
                      return std::make_unique<Cyclic>(database);
                  },
                  std::make_unique<Csids>(repr, parsed.num_vars, default_seed,
                                          default_csids_config()),
                  std::make_unique<NoRestarts>(), nullptr, config);
    for (Clause& clause : parsed.clauses) {
        solver.add_starting_clause(std::move(clause));
    }
    const Solution solution = solver.solve();
    REQUIRE(solution.result == SolveResult::unsat);
    return solver.stats();
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
        DeletionConfig by_sqrt = default_deletion_config();
        by_sqrt.cadence_coefficient = 0.5;
        by_sqrt.cadence_offset = 0;
        CHECK(conflicts_between_deletion_passes(by_sqrt, 10000) == 50);
        CHECK(conflicts_between_deletion_passes(by_sqrt, 30) == 3); // 2.74
        CHECK(conflicts_between_deletion_passes(by_sqrt, 0) == 1);
    }

    SUBCASE("the offset is added to the conflicts under the square root") {
        DeletionConfig offset = default_deletion_config();
        offset.cadence_coefficient = 0.5;
        offset.cadence_offset = 400;
        CHECK(conflicts_between_deletion_passes(offset, 0) == 10);
        CHECK(conflicts_between_deletion_passes(offset, 9600) == 50);
    }

    SUBCASE("the defaults") {
        const DeletionConfig defaults = default_deletion_config();
        CHECK(conflicts_between_deletion_passes(defaults, 0) == 18);         // 17.68
        CHECK(conflicts_between_deletion_passes(defaults, 100) == 18);       // 17.85
        CHECK(conflicts_between_deletion_passes(defaults, 995000) == 250);
        CHECK(conflicts_between_deletion_passes(defaults, 1000000) == 251);  // 250.62
    }

    SUBCASE("the default coefficient with no offset") {
        DeletionConfig no_offset = default_deletion_config();
        no_offset.cadence_offset = 0;
        CHECK(conflicts_between_deletion_passes(no_offset, 0) == 1);
        CHECK(conflicts_between_deletion_passes(no_offset, 100) == 3);
        CHECK(conflicts_between_deletion_passes(no_offset, 1000000) == 250);
    }
}

TEST_CASE_TEMPLATE("deletion: a use as a reason marks a learned clause used", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const std::unique_ptr<ClauseDatabase> database = make_database(repr, 8, {6});

    SUBCASE("the use is recorded for the next pass") {
        update_with_reason_use(database->record(1));
        CHECK(database->record(1).used_since_deletion);
        CHECK(database->record(1).lbd == 6.0);
    }

    SUBCASE("a starting clause records nothing") {
        update_with_reason_use(database->record(0));
        CHECK_FALSE(database->record(0).used_since_deletion);
    }
}

TEST_CASE_TEMPLATE("deletion: a locked clause is neither deleted nor counted", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    // Four candidates under ids 1..4, the first of them the worst by LBD.
    const std::unique_ptr<ClauseDatabase> database = make_database(repr, 8, {12, 11, 10, 9});

    SUBCASE("the reason a trail row names survives its own key") {
        const Trail trail = make_trail_with_reasons(repr, 8, {ClauseId{1}});
        const DeletionPlan plan = plan_for(*database, trail, DeletionKey::lbd);

        // Of the three unlocked candidates, only the worst goes.
        CHECK(plan.deleted == std::vector<ClauseId>{2});
        CHECK_FALSE(contains(plan.deleted, ClauseId{1}));
    }

    SUBCASE("without the lock the same clause is the first to go") {
        const Trail trail(repr, 8);
        const DeletionPlan plan = plan_for(*database, trail, DeletionKey::lbd);
        CHECK(plan.deleted == std::vector<ClauseId>{1, 2});
    }

    SUBCASE("a locked clause with passes left does not age") {
        const std::unique_ptr<ClauseDatabase> allowed = make_database(repr, 8, {5, 5});
        const Trail trail = make_trail_with_reasons(repr, 8, {ClauseId{1}});

        const DeletionPlan plan = plan_for(*allowed, trail, DeletionKey::lbd);
        CHECK(plan.unused_passes[1] == 0);
        CHECK(plan.unused_passes[2] == 1);
    }

    SUBCASE("a locked clause used since the last pass starts its count over") {
        const std::unique_ptr<ClauseDatabase> allowed = make_database(repr, 8, {5});
        allowed->record(1).unused_passes = 1;
        allowed->record(1).used_since_deletion = true;
        const Trail trail = make_trail_with_reasons(repr, 8, {ClauseId{1}});
        CHECK(plan_for(*allowed, trail, DeletionKey::lbd).unused_passes[1] == 0);
    }

    SUBCASE("the counts follow the active ids, whatever order the trail names its reasons in") {
        const std::unique_ptr<ClauseDatabase> allowed = make_database(repr, 8, {5, 5, 5, 5, 5});
        allowed->deactivate_clause(2);
        const Trail trail = make_trail_with_reasons(repr, 8, {ClauseId{5}, ClauseId{1}});

        // Ids 0, 1, 3, 4, 5: the starting clause, then 1 and 5 locked, 3 and 4 aging.
        const DeletionPlan plan = plan_for(*allowed, trail, DeletionKey::lbd);
        CHECK(plan.unused_passes == std::vector<std::size_t>{0, 0, 1, 1, 0});
        CHECK(plan.deleted.empty());
    }
}

TEST_CASE_TEMPLATE("deletion: running out of unused passes", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    // LBD 5 allows two unused passes, LBD 4 four.
    const std::unique_ptr<ClauseDatabase> database = make_database(repr, 8, {5, 4});
    const Trail trail(repr, 8);
    const auto plan_evicting_all = [&] {
        return plan_for(*database, trail, DeletionKey::lbd, nullptr, 1.0);
    };

    SUBCASE("the count accrues a pass at a time, to what the LBD allows") {
        const DeletionPlan first = plan_evicting_all();
        CHECK(first.deleted.empty());
        CHECK(first.unused_passes[1] == 1);
        CHECK(first.unused_passes[2] == 1);

        database->record(1).unused_passes = 1;
        database->record(2).unused_passes = 3;
        const DeletionPlan second = plan_evicting_all();
        CHECK(second.deleted.empty());
        CHECK(second.unused_passes[1] == 2);
        CHECK(second.unused_passes[2] == 4);
    }

    SUBCASE("a clause unused for its allowed passes is a candidate at the next pass") {
        database->record(1).unused_passes = 2;
        database->record(2).unused_passes = 4;
        const DeletionPlan plan = plan_evicting_all();
        CHECK(plan.deleted == std::vector<ClauseId>{1, 2});
        CHECK(plan.unused_passes[1] == 2);
        CHECK(plan.unused_passes[2] == 4);

        // Of two candidates the default rate evicts one, the worse by LBD.
        CHECK(plan_for(*database, trail, DeletionKey::lbd).deleted == std::vector<ClauseId>{1});
    }

    SUBCASE("a use resets the count") {
        database->record(1).unused_passes = 1;
        database->record(1).used_since_deletion = true;
        CHECK(plan_evicting_all().unused_passes[1] == 0);
    }

    SUBCASE("a use protects a clause at its allowance") {
        database->record(1).unused_passes = 2;
        database->record(2).unused_passes = 4;
        database->record(1).used_since_deletion = true;
        const DeletionPlan plan = plan_evicting_all();
        CHECK(plan.deleted == std::vector<ClauseId>{2});
        CHECK(plan.unused_passes[1] == 0);
    }

    SUBCASE("a higher wait allows more passes") {
        database->record(1).unused_passes = 2;
        const std::unique_ptr<Csids> csids = csids_with_bumps(*database, trail, {});
        DeletionConfig config = default_deletion_config();
        config.wait = 7.0;
        config.eviction_rate = 1.0;
        config.key = DeletionKey::lbd;
        const DeletionPlan plan = build_deletion_plan(DeletionState{
            .database = *database, .trail = trail, .config = config, .activity = *csids});
        CHECK(plan.deleted.empty());
        CHECK(plan.unused_passes[1] == 3);
    }
}

TEST_CASE_TEMPLATE("deletion: a use as a reason and a deletion candidate", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    // LBD 5 allows two unused passes, LBD 9 none.
    const std::unique_ptr<ClauseDatabase> database = make_database(repr, 8, {5, 9});
    const Trail trail(repr, 8);
    const auto plan_evicting_all = [&] {
        return plan_for(*database, trail, DeletionKey::lbd, nullptr, 1.0);
    };
    REQUIRE(plan_evicting_all().deleted == std::vector<ClauseId>{2});

    SUBCASE("a clause at its allowance is not a candidate at a pass it was used before") {
        database->record(1).unused_passes = 2;
        use_as_reason(database->record(1), 5);
        const DeletionPlan plan = plan_evicting_all();
        CHECK(plan.deleted == std::vector<ClauseId>{2});
        CHECK(plan.unused_passes[1] == 0);
    }

    SUBCASE("a clause whose LBD allows none survives a pass it was used before, and only that") {
        use_as_reason(database->record(2), 9);
        REQUIRE(database->record(2).lbd == 9.0);
        CHECK(plan_evicting_all().deleted.empty());

        database->record(2).used_since_deletion = false;
        CHECK(plan_evicting_all().deleted == std::vector<ClauseId>{2});
    }

    SUBCASE("a clause whose LBD allowed none is allowed some once its LBD does") {
        use_as_reason(database->record(2), 3);
        CHECK(database->record(2).lbd == 6.0);
        database->record(2).used_since_deletion = false;
        const DeletionPlan plan = plan_evicting_all();
        CHECK(plan.deleted.empty());
        CHECK(plan.unused_passes[2] == 1);
    }

    SUBCASE("a use at too high an LBD protects the clause only at the next pass") {
        database->record(1).unused_passes = 1;
        use_as_reason(database->record(1), 11);
        REQUIRE(database->record(1).lbd == 8.0);
        CHECK(plan_evicting_all().deleted == std::vector<ClauseId>{2});

        database->record(1).used_since_deletion = false;
        database->record(1).unused_passes = 0;
        CHECK(plan_evicting_all().deleted == std::vector<ClauseId>{1, 2});
    }

    SUBCASE("a use keeps the count for the pass to reset") {
        database->record(1).unused_passes = 1;
        use_as_reason(database->record(1), 5);
        CHECK(database->record(1).unused_passes == 1);
        CHECK(plan_evicting_all().unused_passes[1] == 0);
    }
}

TEST_CASE_TEMPLATE("deletion: what the keys order", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Trail trail(repr, 8);

    SUBCASE("lbd deletes the highest LBD, and the older id of a tie") {
        const std::unique_ptr<ClauseDatabase> database = make_database(repr, 8, {9, 12, 12, 10});
        const DeletionPlan plan = plan_for(*database, trail, DeletionKey::lbd);
        // Ordered 2, 3, 4, 1; the first two are evicted, reported ascending.
        CHECK(plan.deleted == std::vector<ClauseId>{2, 3});
    }

    SUBCASE("lbd reads the averaged LBD") {
        const std::unique_ptr<ClauseDatabase> database = make_database(repr, 8, {9, 9});
        database->record(2).lbd = 9.5;
        CHECK(plan_for(*database, trail, DeletionKey::lbd).deleted == std::vector<ClauseId>{2});
    }

    SUBCASE("activity deletes the lowest activity, and the older id of a tie") {
        const std::unique_ptr<ClauseDatabase> database = make_database(repr, 8, {9, 9, 9, 9});
        const std::unique_ptr<Csids> activity =
            csids_with_bumps(*database, trail, {{1, 4}, {2, 1}, {3, 1}, {4, 8}});
        const DeletionPlan plan =
            plan_for(*database, trail, DeletionKey::activity, activity.get());
        CHECK(plan.deleted == std::vector<ClauseId>{2, 3});
    }

    SUBCASE("the two keys disagree, so each is really being read") {
        const std::unique_ptr<ClauseDatabase> database = make_database(repr, 8, {12, 11});
        const std::unique_ptr<Csids> activity =
            csids_with_bumps(*database, trail, {{1, 9}, {2, 1}});
        CHECK(plan_for(*database, trail, DeletionKey::lbd).deleted ==
              std::vector<ClauseId>{1});
        CHECK(plan_for(*database, trail, DeletionKey::activity, activity.get()).deleted ==
              std::vector<ClauseId>{2});
    }
}

TEST_CASE_TEMPLATE("deletion: the eviction rate is the fraction of the candidates deleted", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Trail trail(repr, 8);
    const std::unique_ptr<ClauseDatabase> even = make_database(repr, 8, {12, 11, 10, 9});
    const std::unique_ptr<ClauseDatabase> odd = make_database(repr, 8, {13, 12, 11, 10, 9});
    const auto evicted = [&](const ClauseDatabase& database, double eviction_rate) {
        return plan_for(database, trail, DeletionKey::lbd, nullptr, eviction_rate).deleted;
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
        auto database = std::make_unique<ClauseDatabase>();
        for (std::size_t index = 0; index < 4; ++index) {
            database->add_starting_clause(make_clause(repr, 8, {{index}}));
        }

        const DeletionPlan plan = plan_for(*database, trail, DeletionKey::lbd);
        CHECK(plan.deleted.empty());
        CHECK(plan.unused_passes[0] == 0);
    }

    SUBCASE("a clause with unused passes left") {
        const std::unique_ptr<ClauseDatabase> database = make_database(repr, 8, {1, 2, 3, 3});
        const DeletionPlan plan = plan_for(*database, trail, DeletionKey::lbd);
        CHECK(plan.deleted.empty());
    }

    SUBCASE("a clause already deleted") {
        const std::unique_ptr<ClauseDatabase> database = make_database(repr, 8, {9, 9, 9, 9});
        database->deactivate_clause(1);
        database->deactivate_clause(2);

        // Two candidates are left, so exactly one goes.
        const DeletionPlan plan = plan_for(*database, trail, DeletionKey::lbd);
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
}

TEST_CASE_TEMPLATE("deletion: a run deletes, and deletes the same way twice", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const RunStats first = solve_deleting(repr, every_conflict);
    const RunStats second = solve_deleting(repr, every_conflict);

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
    const RunStats counters = solve_deleting(repr, every_conflict, 62.0);
    CHECK(counters.num_deletion_passes != 0);
    CHECK(counters.num_deleted_clauses == 0);
}

TEST_CASE_TEMPLATE("deletion: a longer pass cadence deletes less often", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const RunStats often = solve_deleting(repr, every_conflict);
    const RunStats seldom = solve_deleting(repr, 1000.0);

    CHECK(often.num_deletion_passes > seldom.num_deletion_passes);
    CHECK(seldom.num_deletion_passes == 0);
    CHECK(seldom.num_deleted_clauses == 0);
}

} // namespace xorcle
