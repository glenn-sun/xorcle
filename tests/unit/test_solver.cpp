#include <doctest/doctest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "core/clause_id.h"
#include "core/solver.h"
#include "core/trail.h"
#include "heuristics/decisions/csids.h"
#include "heuristics/restarts/luby.h"
#include "heuristics/restarts/restart_policy.h"
#include "heuristics/selection/clause_selector.h"
#include "heuristics/selection/cyclic.h"
#include "io/dimacs.h"
#include "util/index_set.h"
#include "util/random.h"

namespace xorcle {

namespace {

// --- The instances ---------------------------------------------------------
// Each is small enough to work out by hand, and every expected verdict below is
// derived in the comment rather than read off a run.

/// @brief `[x1 = 1] ^ [x1 = 0]`: UNSAT by root propagation alone, no decisions.
constexpr std::string_view root_unsat_instance = "p cnf 1 2\n"
                                                 "1 0\n"
                                                 "-1 0\n";

/// @brief Three unit clauses fixing every variable, so SAT with no decisions:
/// `x1 = 1`, `x2 = 0`, `x1 + x3 = 1`, whose only solution is `1 0 0`.
constexpr std::string_view unit_sat_instance = "p cnf 3 3\n"
                                               "1 0\n"
                                               "-2 0\n"
                                               "1+3 0\n";

/// @brief SAT, but only after deciding: no clause is unit to begin with.
/// `[x1 v x2] ^ [-x1 v -x2] ^ [[x1 + x2 = 1] v x3]` forces exactly one of
/// x1, x2 and satisfies the third clause outright, leaving x3 free to decide.
constexpr std::string_view search_sat_instance = "p cnf 3 3\n"
                                                 "1 2 0\n"
                                                 "-1 -2 0\n"
                                                 "1+2 3 0\n";

/// @brief All eight sign patterns over three variables: UNSAT, and every
/// assignment is ruled out by exactly one clause, so the search must decide.
constexpr std::string_view search_unsat_instance = "p cnf 3 8\n"
                                                   "1 2 3 0\n"
                                                   "1 2 -3 0\n"
                                                   "1 -2 3 0\n"
                                                   "1 -2 -3 0\n"
                                                   "-1 2 3 0\n"
                                                   "-1 2 -3 0\n"
                                                   "-1 -2 3 0\n"
                                                   "-1 -2 -3 0\n";

/// @brief SAT, with three root-level determined variables for compaction to
/// eliminate.
///
/// Root propagation fixes `x1 = 1`, `x2 = 0` and `x4 + x5 = 1`, determining
/// variables 0, 1 and 3, so the compacted space keeps only x3 and x5 and the
/// surviving `[x3 v x4]`, `[-x3 v -x4]` still need a decision. Every solution
/// has `x3 != x4` and `x5 = x4 + 1`.
constexpr std::string_view compaction_sat_instance = "p cnf 5 5\n"
                                                     "1 0\n"
                                                     "-2 0\n"
                                                     "4+5 0\n"
                                                     "3 4 0\n"
                                                     "-3 -4 0\n";

// --- Checking an answer ----------------------------------------------------

/// @brief Whether an assignment satisfies one stored clause row.
bool row_holds(const Equation& row, const IndexSet& assignment) {
    bool parity = false;
    for (const std::size_t bit : bits_of(row)) {
        parity ^= bit == assignment.size() ? true : assignment.test(bit);
    }
    return !parity;
}

/// @brief Whether an assignment satisfies a clause.
///
/// The rows are the clause's linear negation, so an equation of the clause
/// holds exactly where its row does *not*.
bool clause_is_satisfied(const Clause& clause, const IndexSet& assignment) {
    for (const Equation& row : clause) {
        if (!row_holds(row, assignment)) {
            return true;
        }
    }
    return false;
}

/// @brief Assert that a reported assignment satisfies every input clause,
/// re-evaluated over the original instance.
void check_satisfies(const std::vector<Clause>& clauses, const Solution& solution,
                     std::size_t num_vars) {
    REQUIRE(solution.result == SolveResult::sat);
    REQUIRE(solution.assignment.has_value());
    REQUIRE(solution.assignment->size() == num_vars);
    for (std::size_t id = 0; id < clauses.size(); ++id) {
        CHECK_MESSAGE(clause_is_satisfied(clauses[id], *solution.assignment),
                      "the reported assignment falsifies clause ", id);
    }
}

// --- Building a run --------------------------------------------------------

/// @brief One configured solver together with the instance it was built from.
struct Run {
    std::size_t num_vars = 0;
    std::vector<Clause> clauses; ///< The input clauses, kept for checking answers.
    std::unique_ptr<Solver> solver; ///< The solver, already loaded with `clauses`.
};

/// @brief Parse an instance and build a solver over it with the CLI's default
/// heuristics, so that a run here is the run `xorcle <file>` performs.
///
/// Null `restarts`/`selector` mean `NoRestarts` and `Cyclic`.
Run make_run(EquationRepr repr, std::string_view instance, const CompactionConfig& compaction = {},
             std::unique_ptr<RestartPolicy> restarts = nullptr,
             std::unique_ptr<ClauseSelector> selector = nullptr) {
    ParsedDimacs parsed = parse_dimacs_string(instance, repr);

    Run run;
    run.num_vars = parsed.num_vars;
    run.clauses = parsed.clauses;
    run.solver = std::make_unique<Solver>(
        repr, parsed.num_vars,
        selector != nullptr ? std::move(selector) : std::make_unique<Cyclic>(parsed.num_vars),
        std::make_unique<Csids>(repr, parsed.num_vars, default_seed),
        restarts != nullptr ? std::move(restarts) : std::make_unique<NoRestarts>(), nullptr,
        compaction);
    for (Clause& clause : parsed.clauses) {
        run.solver->add_starting_clause(std::move(clause));
    }
    return run;
}

/// @brief A `Luby` policy on the smallest unit budget -- 1, 1, 2, 1, ... -- so a
/// handful of conflicts restart several times.
std::unique_ptr<RestartPolicy> tiny_luby_restarts(bool reuse_trail = false) {
    RestartConfig config;
    config.method = RestartMethod::luby;
    config.luby_unit_conflicts = 1;
    config.reuse_trail = reuse_trail;
    return std::make_unique<Luby>(config);
}

// --- Watching the backjump -------------------------------------------------

/// @brief How many dimensions a clause adds to a span, i.e. the dimension of
/// `(neg(C) + span(U)) / span(U)`; the clause propagates exactly when this is 1.
std::size_t dimension_outside_span(const Clause& clause, const RowSpace& span) {
    RowSpace extended = span;
    std::size_t outside = 0;
    for (const Equation& row : clause) {
        if (extended.add_row_get_index(row).has_value()) {
            ++outside;
        }
    }
    return outside;
}

/// @brief The level a clause asserts at, recomputed by backjumping a copy of
/// the conflicting trail.
std::size_t asserting_level_of(const Clause& clause, const Trail& conflict_trail) {
    for (std::size_t level = 0; level <= conflict_trail.level(); ++level) {
        Trail backjumped = conflict_trail;
        backjumped.resize(level + 1);
        if (dimension_outside_span(clause, backjumped.span()) == 1) {
            return level;
        }
    }
    FAIL("the learned clause asserts at no level of the conflicting trail");
    return conflict_trail.level();
}

/// @brief One conflict, as the clause selector sees it.
struct Backjump {
    std::size_t level_before = 0; ///< Decision level when the conflict was reached.
    std::size_t level_after = 0; ///< Decision level the solver backjumped to.
    std::size_t removed_levels = 0; ///< What the solver told the selector it discarded.
    std::size_t asserting_level = 0; ///< Where the learned clause actually asserts.
    std::size_t lbd = 0; ///< The LBD the solver told the selector.
};

/// @brief A cyclic selector that records every backjump it is told about.
class RecordingSelector final : public ClauseSelector {
public:
    /// @brief The next active clause in cyclic order, or `std::nullopt` at quiescence.
    std::optional<ClauseId> next() override {
        if (clauses_.empty() || count_ >= clauses_.size()) {
            count_ = 0;
            return std::nullopt;
        }
        for (std::size_t attempts = 0; attempts < clauses_.size(); ++attempts) {
            const ClauseId current = id_;
            id_ = (id_ + 1) % clauses_.size();
            ++count_;
            if (is_clause_active(current)) {
                return current;
            }
            if (count_ >= clauses_.size()) {
                count_ = 0;
                return std::nullopt;
            }
        }
        count_ = 0;
        return std::nullopt;
    }

    void update_with_propagation(const Equation& reduced_eqn, const Trail& trail) override {
        last_trail_ = trail;
        ClauseSelector::update_with_propagation(reduced_eqn, trail);
    }

    void update_with_decision(const Equation& reduced_eqn, const Trail& trail) override {
        last_trail_ = trail;
        ClauseSelector::update_with_decision(reduced_eqn, trail);
    }

    /// @brief Record the backjump, then store the clause.
    ///
    /// The trail passed here is the backjumped one; the last trail seen before
    /// it is the conflicting one.
    void add_asserting_clause(Clause clause, const Trail& trail,
                              const AssertingClauseInfo& info) override {
        REQUIRE(last_trail_.has_value());
        backjumps.push_back(Backjump{last_trail_->level(), trail.level(), info.removed_levels,
                                     asserting_level_of(clause, *last_trail_), info.lbd});
        ClauseSelector::add_asserting_clause(std::move(clause), trail, info);
    }

    /// @brief Refresh the quiescence budget, as `Cyclic` does.
    void update_with_new_equation(const Equation&, const Trail&) override { count_ = 0; }

    /// @brief Every conflict this selector was told about, oldest first.
    std::vector<Backjump> backjumps;

protected:
    /// @brief Store a clause and make it the next one tried.
    void add_clause(Clause clause) override {
        ClauseSelector::add_clause(std::move(clause));
        id_ = clauses_.size() - 1;
        count_ = 0;
    }

private:
    ClauseId id_ = 0;
    std::size_t count_ = 0;
    std::optional<Trail> last_trail_;
};

} // namespace

TEST_CASE_TEMPLATE("solver: a root conflict, proved with no decisions", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Run run = make_run(repr, root_unsat_instance);

    const Solution solution = run.solver->solve();
    CHECK(solution.result == SolveResult::unsat);
    CHECK_FALSE(solution.assignment.has_value());

    // Both clauses are unit, so both propagate at the root: `[x1 = 1]` and then
    // `[x1 = 0]`, whose sum is 1.
    CHECK(run.solver->level() == 0);
    CHECK(run.solver->decision_stats().decisions == 0);
    CHECK(run.solver->counters().num_conflicts == 1);
    CHECK(run.solver->counters().num_propagations == 2);
    CHECK(run.solver->counters().num_restarts == 0);
    // Nothing was learned: a root conflict ends the run before analysis.
    CHECK(run.solver->num_clauses() == run.clauses.size());
}

TEST_CASE_TEMPLATE("solver: SAT decided entirely by root propagation", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Run run = make_run(repr, unit_sat_instance);

    const Solution solution = run.solver->solve();
    check_satisfies(run.clauses, solution, run.num_vars);

    // The unique solution: x1 = 1, x2 = 0, and x3 = x1 + 1 = 0.
    CHECK(solution.assignment->test(0));
    CHECK_FALSE(solution.assignment->test(1));
    CHECK_FALSE(solution.assignment->test(2));

    CHECK(run.solver->counters().num_propagations == 3);
    CHECK(run.solver->counters().num_conflicts == 0);
    CHECK(run.solver->decision_stats().decisions == 0);
}

TEST_CASE_TEMPLATE("solver: SAT found by search", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Run run = make_run(repr, search_sat_instance);

    const Solution solution = run.solver->solve();
    check_satisfies(run.clauses, solution, run.num_vars);

    // The first two clauses force exactly one of x1, x2, and no clause is unit
    // at the root, so the answer cannot be reached without deciding.
    CHECK(solution.assignment->test(0) != solution.assignment->test(1));
    CHECK(run.solver->decision_stats().decisions > 0);
}

TEST_CASE_TEMPLATE("solver: UNSAT found by search", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Run run = make_run(repr, search_unsat_instance);

    const Solution solution = run.solver->solve();
    CHECK(solution.result == SolveResult::unsat);
    CHECK_FALSE(solution.assignment.has_value());

    // Eight clauses over three variables, each ruling out one assignment: the
    // solver has to decide, conflict, and learn its way through them.
    CHECK(run.solver->decision_stats().decisions > 0);
    CHECK(run.solver->num_clauses() > run.clauses.size());
}

TEST_CASE_TEMPLATE("solver: the counters a search run accumulates", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Run run = make_run(repr, search_unsat_instance);
    CHECK(run.solver->counters().num_conflicts == 0);

    const Solution solution = run.solver->solve();
    REQUIRE(solution.result == SolveResult::unsat);

    const SolverCounters& counters = run.solver->counters();
    CHECK(counters.num_attempt_props > 0);
    CHECK(counters.num_propagations > 0);
    CHECK(counters.num_conflicts > 0);
    // A clause is only ever handed to the propagator by the selector, and only a
    // handed-out clause can propagate or move a watch.
    CHECK(counters.num_attempt_props >= counters.num_propagations);
    CHECK(counters.num_attempt_props >= counters.num_watch_updates);
    // Every clause has three rows, so the propagator has somewhere to move a
    // watch to.
    CHECK(counters.num_watch_updates > 0);
    CHECK(counters.num_restarts == 0);

    // Not hand-derived: measured on this instance with the heuristics `make_run`
    // fixes (CSIDS at seed 0, cyclic selection, falsify phase, compaction on).
    CHECK(counters.num_conflicts == 4);
    CHECK(run.solver->decision_stats().decisions == 3);
}

TEST_CASE("solver: the two representations agree on what the search did") {
    // Deliberately not a `TEST_CASE_TEMPLATE`: the claim is about the two
    // representations together.
    Run dense = make_run(EquationRepr::dense, search_unsat_instance);
    Run sparse = make_run(EquationRepr::sparse, search_unsat_instance);

    const Solution dense_solution = dense.solver->solve();
    const Solution sparse_solution = sparse.solver->solve();
    CHECK(dense_solution.result == sparse_solution.result);

    const SolverCounters& dense_counters = dense.solver->counters();
    const SolverCounters& sparse_counters = sparse.solver->counters();
    CHECK(dense_counters.num_conflicts == sparse_counters.num_conflicts);
    CHECK(dense_counters.num_propagations == sparse_counters.num_propagations);
    CHECK(dense_counters.num_attempt_props == sparse_counters.num_attempt_props);
    CHECK(dense_counters.num_watch_updates == sparse_counters.num_watch_updates);
    CHECK(dense_counters.num_restarts == sparse_counters.num_restarts);
}

TEST_CASE_TEMPLATE("solver: compaction runs and the answer comes back in the original space", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    CompactionConfig compaction_off;
    compaction_off.enabled = false;
    Run without = make_run(repr, compaction_sat_instance, compaction_off);
    const Solution without_solution = without.solver->solve();
    check_satisfies(without.clauses, without_solution, without.num_vars);

    Run with = make_run(repr, compaction_sat_instance);
    const Solution with_solution = with.solver->solve();

    // The root trail determines variables 0, 1 and 3, leaving x3 and x5 to
    // search in, while `num_vars()` still reports the original space.
    REQUIRE(with.solver->num_vars() == 5);

    // Same verdict, and an assignment over the *original* five variables.
    CHECK(with_solution.result == without_solution.result);
    check_satisfies(with.clauses, with_solution, with.num_vars);

    // The back-substituted variables, checked against the instance by hand:
    // x1 = 1 and x2 = 0 are the root units, and x4 = x5 + 1.
    CHECK(with_solution.assignment->test(0));
    CHECK_FALSE(with_solution.assignment->test(1));
    CHECK(with_solution.assignment->test(3) != with_solution.assignment->test(4));
    CHECK(with_solution.assignment->test(2) != with_solution.assignment->test(3));
}

TEST_CASE_TEMPLATE("solver: compaction does not change an UNSAT verdict", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    CompactionConfig compaction_off;
    compaction_off.enabled = false;
    Run without = make_run(repr, search_unsat_instance, compaction_off);
    Run with = make_run(repr, search_unsat_instance);

    CHECK(without.solver->solve().result == SolveResult::unsat);
    CHECK(with.solver->solve().result == SolveResult::unsat);
}

TEST_CASE_TEMPLATE("solver: restarting does not change the verdict", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Run restarting = make_run(repr, search_unsat_instance, CompactionConfig{},
                              tiny_luby_restarts());

    CHECK(restarting.solver->solve().result == SolveResult::unsat);
    CHECK(restarting.solver->counters().num_restarts > 0);

    Run satisfiable =
        make_run(repr, search_sat_instance, CompactionConfig{}, tiny_luby_restarts());
    const Solution solution = satisfiable.solver->solve();
    check_satisfies(satisfiable.clauses, solution, satisfiable.num_vars);
}

TEST_CASE_TEMPLATE("solver: a backjump removes the levels it says it removes", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    // `removed_levels` is what the solver discarded, which is the complement of
    // the asserting level only when no restart fired. The restarting subcase is
    // where the two quantities come apart.
    auto record = [&](std::unique_ptr<RestartPolicy> restarts) {
        auto selector = std::make_unique<RecordingSelector>();
        RecordingSelector& recorder = *selector;
        Run run = make_run(repr, search_unsat_instance, CompactionConfig{},
                           std::move(restarts), std::move(selector));
        CHECK(run.solver->solve().result == SolveResult::unsat);
        return recorder.backjumps;
    };

    SUBCASE("without restarts the removed count is the asserting level's complement") {
        const std::vector<Backjump> backjumps = record(nullptr);
        REQUIRE_FALSE(backjumps.empty());
        for (const Backjump& backjump : backjumps) {
            CHECK(backjump.level_after + backjump.removed_levels == backjump.level_before);
            CHECK(backjump.level_after == backjump.asserting_level);
            CHECK(backjump.removed_levels == backjump.level_before - backjump.asserting_level);
        }
    }

    SUBCASE("with restarts and no trail reuse it is every non-root level") {
        const std::vector<Backjump> backjumps = record(tiny_luby_restarts());
        REQUIRE_FALSE(backjumps.empty());

        std::size_t discriminating = 0;
        for (const Backjump& backjump : backjumps) {
            CHECK(backjump.level_after + backjump.removed_levels == backjump.level_before);
            if (backjump.level_after == 0 && backjump.asserting_level > 0) {
                // A restart over a clause that asserts above the root, where
                // `level_before - asserting_level` is strictly smaller than the
                // number of levels actually discarded.
                CHECK(backjump.removed_levels == backjump.level_before);
                CHECK(backjump.removed_levels >
                      backjump.level_before - backjump.asserting_level);
                ++discriminating;
            }
        }
        // The case that tells the two formulas apart really did occur.
        CHECK(discriminating > 0);
    }

    SUBCASE("trail reuse stops a restart short of the root, never above the clause") {
        const std::vector<Backjump> backjumps = record(tiny_luby_restarts(true));
        REQUIRE_FALSE(backjumps.empty());

        for (const Backjump& backjump : backjumps) {
            CHECK(backjump.level_after + backjump.removed_levels == backjump.level_before);
            // Reuse chooses within what the conflict already allows, so a
            // restart still lands at or below the asserting level.
            CHECK(backjump.level_after <= backjump.asserting_level);
        }
    }
}

TEST_CASE_TEMPLATE("solver: the learned clause's LBD reaches the selector", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    auto selector = std::make_unique<RecordingSelector>();
    RecordingSelector& recorder = *selector;
    Run run = make_run(repr, search_unsat_instance, CompactionConfig{}, nullptr,
                       std::move(selector));
    const std::size_t num_input_clauses = run.clauses.size();
    CHECK(run.solver->solve().result == SolveResult::unsat);

    const std::vector<Backjump>& backjumps = recorder.backjumps;
    REQUIRE(recorder.num_clauses() == num_input_clauses + backjumps.size());
    REQUIRE_FALSE(backjumps.empty());

    for (ClauseId id = 0; id < num_input_clauses; ++id) {
        CHECK_FALSE(recorder.is_clause_learned(id));
        CHECK(recorder.clause_tier(id) == ClauseTier::core);
    }
    for (std::size_t index = 0; index < backjumps.size(); ++index) {
        const ClauseId id = num_input_clauses + index;
        const std::size_t lbd = backjumps[index].lbd;
        CHECK(recorder.is_clause_learned(id));
        CHECK(recorder.clause_lbd(id) == lbd);
        CHECK(recorder.clause_tier(id) == recorder.tier_for_lbd(lbd));
        // The clause is derived from the levels of the conflicting trail.
        CHECK(lbd <= backjumps[index].level_before);
    }
}

} // namespace xorcle
