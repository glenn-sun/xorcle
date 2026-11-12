#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/reduce_result.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "cli/options.h"
#include "core/clause_id.h"
#include "core/solver.h"
#include "core/trail.h"
#include "heuristics_test_support.h"
#include "heuristics/selection/clause_selector.h"
#include "io/dimacs.h"
#include "io/proof_writer.h"
#include "io_test_support.h"
#include "util/index_set.h"

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
///
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

/// @brief The random_kxnf instance with n=13, k=3 and seed 2, SAT, on which a cyclic
/// search at `--propagate-miss-rate=0.5` and `--seed=9` decides against a clause it
/// left unit, and undoes that decision.
constexpr std::string_view missed_unit_instance =
    "p cnf 13 70\n"
    "-1+2+3+4+9+11+12+13 -3+5+7+8+10+12 -1+2+3+4+5+7+8+10+12 0\n"
    "4+6+9+10+11+13 -8+9+11+12+13 -2+3+5+7+8+9+10+11+13 0\n"
    "-3+4+5+8+9+11+12 1+5+7+8+9+10+12+13 -1+2+5+6+7+8+9+12 0\n"
    "2+3+4+6+7+8+10+11 -1+2+3+4+5+6+7+8+9+10+11 3+5+6+7+8+10+11+13 0\n"
    "-1+2+4+5+8+9+12 1+2+4+5+7+10+12+13 2+3+4+11 0\n"
    "-1+5+6+11+12 2+3+6+8+9+12 -2+3+4+5+6 0\n"
    "-1+2+3+5+7+9+10+11 -3+4+6+10+11+12 -1+2+5+8+9+13 0\n"
    "2+4+5+6+8 -3+5+6+7+8+9+11 -5+7+8+9+10+11 0\n"
    "3+4+12+13 6+11+12 2+3+6+7+8+9+11+12 0\n"
    "-1+3+4+6+8+9+10+12 -1+5+6+7+9+12+13 -4+5+11+12+13 0\n"
    "2+6+10+11+12 5+6+9+13 -2+5+8 0\n"
    "1+3+5+7+10+11+13 -1+2+6+9+13 2+3+4+5+7+9+10 0\n"
    "-2+4+6+7+8+9+11+12+13 -5+6+8+10+12 -1+2+3+4+5+8+12+13 0\n"
    "1+5+9+10+11+12+13 3+5+6+9+12+13 -2+3+5+6+7+8+12 0\n"
    "-1+3+4+7+13 1+3+4+7+8+10+11+12+13 -1+2+3+4+6+7+10+11+13 0\n"
    "1+2+6+7+8+9+11+13 3+5+6+7+9+11+12 -1+2+3+4+7+8+9+10+13 0\n"
    "-3+4+8+9+12 -1+2+6+8+9+10+11+13 -2+3+4+8+9+12 0\n"
    "-1+4+5+6+7+8+11+12+13 1+2+8+10 -1+3+7+8+10+13 0\n"
    "3+6+7+8+9+13 -3+4+5+7+8+10+11+12 1+4+5+6+7+9+10+11 0\n"
    "-2+4+6+9+12+13 -2 3+5+6+8+9+11 0\n"
    "2+6+7+8+13 -3+4+5+8+9+10+13 -1+2+3+4+5+6+7+9+11+12+13 0\n"
    "1+2+3+4+5+6+8+9+10+11 5+6+11+12 -1+4+11+12+13 0\n"
    "1+2+5+8+9+10+12 1+2+7+10+12 10+12+13 0\n"
    "2+4+5+6+8+11 -1+5+6+7+10+11+12+13 -3+6+9+10+12+13 0\n"
    "1+3+4+5+6+7+9+11+12 -3+6+9+10+13 -1+2+3+9+10 0\n"
    "7+8+11+12+13 -2+3+9+12+13 4+5+7+8+9+11+12+13 0\n"
    "-6+12 -2+5+6+7+9+10+11+12+13 1+2+4+6+7+9+10 0\n"
    "9+11+13 -4+5+7+11+13 1+4+5+6+11+13 0\n"
    "1+3+5+6+8+10+11+12+13 -1+3+6+8+11+12+13 2+3+5+6+7+8+10+12 0\n"
    "-1+3+4+6+7+8+9+12 1+5+6+8+9+10+12+13 5+6+8+9+10+11+12 0\n"
    "-1+3+5+6+8+10 2+4+8+10+11 3+4+5+8+9+10+11+12+13 0\n"
    "1+2+4+6+7+8+12+13 2+6+8+9+10+12 -3+4+7+8+13 0\n"
    "-2+4+5+7+9+13 -1+4+6+7+8+9+12+13 1+3+4+5+7+10+12 0\n"
    "1+4+5+6+8+11 -2+9+10+11+12 1+2+3+7+11 0\n"
    "1+3+4+5+7+9+11+12 6+11+13 -1+3+4+6+7+8+10+11 0\n"
    "-4+5+9+10+12 1+3+6+7+10+11+12+13 -1+3+5+6+7+13 0\n"
    "-3+4+5+6+7+9+10+12+13 -1+2+7+10+13 -1+2+3+5+9+10+12 0\n"
    "1+2+3+7+10+11+12 2+6+9+10+12+13 2+3+4+5+6+9+10 0\n"
    "-2+5+6+7+9+10+12 1+2+3+4+5+9+10+12 3+5+7+9+10+12 0\n"
    "6+9+10+11+13 1+2+6+7+9+10+11+13 1+5+8+9+10+12+13 0\n"
    "-1+2+3+7+8+9+13 -1+3+4+5+6+7+11+13 -3+4+5+6+9+10+11+12+13 0\n"
    "2+3+4+9+10+11 2+3+4+5+9+11+12 1+2+3+4+5+7+9+12 0\n"
    "-5+6+9+11 -1+2+4+6+7+10+11 2+9+10 0\n"
    "-2+3+4+5+7+9+10+11 -1+6+7+9+10+11 -3+5+9+11+12 0\n"
    "2+3+4+5+6+9+13 -1+3+4+8+10+12 -2+3+4+5+7+9+11+12 0\n"
    "2+3+6+9+10+12 2+3+7+11 -3+7+8+9+10+12+13 0\n"
    "4+6+8+9+10+11+12+13 2+4+5+6+10+13 -2+4+6+9+12 0\n"
    "1+3+5+6+7+9+12+13 -7+9+10+11+12+13 -1+2+3+7+9+10+13 0\n"
    "-2+5+7+9+10+11 1+3+5+8+12+13 -1+2+5+7+10+11+12 0\n"
    "-2+3+4+5+6+7+10+11+13 -1+2+4+6+8+10 -1+3+4+7+9+10+12 0\n"
    "-1+3+4+7+8+12+13 -2+4+5+6+7+10+11+12+13 3+4+5+6+7+9+10+13 0\n"
    "2+4 1+3+4+7 -1+3+4+5+6+10+12 0\n"
    "7+9 -1+2+4+6+10+12 2+3+4+5+8+12+13 0\n"
    "1+3+6+7+10+11+13 -1+2+4+6+9+11+12 2+3+4+5+6+8+9+12 0\n"
    "1+3+5+9+10+11+13 1+2+4+7+10+11+13 -1+4+5+6+9+11+13 0\n"
    "-3+5+7+9+10+11+13 -1+3+4+6+8+12+13 -2+3+4+7+10+12 0\n"
    "-1+2+3+13 3+4+5+8+12 -2+3+7+8+11+12 0\n"
    "1+4+5+7+8+9 -1+2+3+4+5+6+9+10+11+12+13 -1+4+9+10+11+12+13 0\n"
    "1+2+10+11+13 1+5+6+7+8+9 -1+3+4+5+7+8+10 0\n"
    "4+5+7+8+10+11+12 -1+3+5+9+11+12 2+3+4+6+10 0\n"
    "-5+6+7+9+12+13 1+3+4+6+8+10+12+13 -1+2+5+6+7+9+10+12+13 0\n"
    "2+3+5+6+8+9+10 -1+10+11 -9+12 0\n"
    "-2+3+4+11+13 -1+5+13 -1+3+4+5+7+8+9+13 0\n"
    "1+3+6+7+9+10+11+12 -1+2+3+5+6+9+10+13 -2+4+6+7+8+9+13 0\n"
    "-1+3+4+5+7+8+10+13 2+3+4+5+8+12+13 -1+3+6+7+9+13 0\n"
    "3+9+10+12+13 -5+6+7+8+9+10+11+13 2+3+4+5+6+8+9+10+11+12 0\n"
    "-5+6+7+9+11+12+13 1+2+4+5+7+9+12+13 -2+4+5+7+8+10+11 0\n"
    "-3+4+5+6+8+9+10 2+4+6+7+8+9 -2+4+7+9+10+12 0\n"
    "-1+2+5+6+7+12+13 -1+2+3+5+10+11+12 -3+4+6+7+8+10 0\n"
    "-1+3+4+5+7+8+11+12+13 -1+4+7+8+9+11 1+2+4+5+11+12 0\n";

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

/// @brief Whether an assignment satisfies a clause, given by its rows over the
/// declared variables.
///
/// The rows are the clause's linear negation, so an equation of the clause
/// holds exactly where its row does *not*.
bool clause_is_satisfied(const std::vector<ProofRow>& rows, const IndexSet& assignment) {
    for (const ProofRow& row : rows) {
        bool parity = false;
        for (const std::size_t bit : row) {
            parity ^= bit == assignment.size() ? true : assignment.test(bit);
        }
        if (parity) {
            return true;
        }
    }
    return false;
}

/// @brief Assert that a reported assignment satisfies every input clause,
/// re-evaluated over the original instance.
void check_satisfies(const std::vector<std::vector<ProofRow>>& clauses, const Solution& solution,
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
    std::vector<Clause> clauses; ///< The input clauses, over the used variables.
    std::vector<std::vector<ProofRow>> rows; ///< The same over the declared variables.
    std::unique_ptr<Solver> solver; ///< The solver, already loaded with `clauses`.
};

/// @brief The heuristics these tests were written against, before the defaults
/// moved on: cyclic selection propagating to a fixed point, and no deletion,
/// subsumption or implication graph.
std::vector<std::string> old_heuristics() {
    return {"--selection=cyclic", "--propagate-miss-rate=0", "--deletion=false",
            "--otf-subsume=false", "--implication-graph=false"};
}

/// @brief `a` followed by `b`.
std::vector<std::string> operator+(std::vector<std::string> a, const std::vector<std::string>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

/// @brief Parse an instance and build the solver `xorcle` builds for it under
/// @p flags, so that a run here is the run `xorcle <flags> <file>` performs.
///
/// The sparse representation runs the cyclic selector unless @p flags choose. BVE is off unless @p flags choose: it solves most of
/// these instances before the first decision.
/// @param make_selector Replaces the selector @p flags name, when non-null.
/// @param proof_writer Not owned; null for no proof.
Run make_run(EquationRepr repr, std::string_view instance,
             const std::vector<std::string>& flags = {},
             ClauseSelectorFactory make_selector = nullptr,
             ProofWriter* proof_writer = nullptr) {
    std::vector<std::string> arguments = {"--bve=false"};
    if (repr == EquationRepr::sparse) {
        arguments.insert(arguments.end(), {"--equation-repr=sparse", "--selection=cyclic"});
    }
    arguments = arguments + flags + std::vector<std::string>{"instance.xnf"};
    std::ostringstream help;
    const std::optional<Options> options = parse_options(arguments, help);
    REQUIRE(options.has_value());

    ParsedDimacs parsed = parse_dimacs_string(instance, repr);
    Run run;
    run.num_vars = parsed.num_vars;
    run.clauses = parsed.clauses;
    run.rows = clause_rows(parsed);
    run.solver =
        make_solver(*options, parsed.num_vars, parsed.used_vars, proof_writer, nullptr,
                    std::move(make_selector));
    for (Clause& clause : parsed.clauses) {
        run.solver->add_starting_clause(std::move(clause));
    }
    return run;
}

/// @brief A `Luby` schedule on the smallest unit budget -- 1, 1, 2, 1, ... -- so a
/// handful of conflicts restart several times.
std::vector<std::string> tiny_luby_restarts() {
    return {"--restart=luby", "--restart-luby-unit=1"};
}

// --- Watching the backjump -------------------------------------------------

/// @brief The level a clause asserts at, recomputed by backjumping a copy of
/// the conflicting trail.
std::size_t asserting_level_of(const Clause& clause, const Trail& conflict_trail) {
    for (std::size_t level = 0; level <= conflict_trail.level(); ++level) {
        Trail backjumped = conflict_trail;
        backjumped.resize(level + 1);
        if (dimensions_outside_span(clause, backjumped.span()) == 1) {
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
    double record_lbd = 0.0; ///< The clause's LBD in the database then.
};

/// @brief A cyclic selector that records every backjump it is told about.
///
/// It runs under `old_heuristics`: it has no attempt budget, and it takes every
/// clause added to it for a conflict's, which subsumption and the implication
/// graph would break.
class RecordingSelector final : public ClauseSelector {
public:
    explicit RecordingSelector(const ClauseDatabase& database) : ClauseSelector(database) {}

    /// @brief The next active clause in cyclic order, or `std::nullopt` at quiescence.
    std::optional<ClauseSelection> next() override {
        if (database_.num_clauses() == 0 || count_ >= database_.num_clauses()) {
            count_ = 0;
            return std::nullopt;
        }
        for (std::size_t attempts = 0; attempts < database_.num_clauses(); ++attempts) {
            const ClauseId current = id_;
            id_ = (id_ + 1) % database_.num_clauses();
            ++count_;
            if (database_.is_clause_active(current)) {
                return ClauseSelection{current, std::nullopt};
            }
            if (count_ >= database_.num_clauses()) {
                count_ = 0;
                return std::nullopt;
            }
        }
        count_ = 0;
        return std::nullopt;
    }

    /// @brief Record the trail and refresh the quiescence budget, as `Cyclic` does.
    void update_with_new_equation(const Equation&, const Trail& trail) override {
        last_trail_ = trail;
        count_ = 0;
    }

    /// @brief Record the backjump, then make the clause the next one tried.
    ///
    /// The trail passed here is the backjumped one; the last trail seen before
    /// it is the conflicting one.
    void update_with_asserting_clause(ClauseId id, const Trail& trail,
                                      const AssertingClauseInfo& info) override {
        REQUIRE(last_trail_.has_value());
        backjumps.push_back(Backjump{last_trail_->level(), trail.level(), info.removed_levels,
                                     asserting_level_of(database_.clause(id), *last_trail_),
                                     info.lbd, database_.record(id).lbd});
        ClauseSelector::update_with_asserting_clause(id, trail, info);
    }

    /// @brief Every conflict this selector was told about, oldest first.
    std::vector<Backjump> backjumps;

    /// @brief Make a new clause the next one tried.
    void update_with_new_clause(ClauseId id) override {
        id_ = id;
        count_ = 0;
    }

private:
    ClauseId id_ = 0;
    std::size_t count_ = 0;
    std::optional<Trail> last_trail_;
};

/// @brief Builds a `RecordingSelector` and points @p recorder at it.
ClauseSelectorFactory recording_selector(RecordingSelector*& recorder) {
    return [&recorder](const ClauseDatabase& database) {
        auto selector = std::make_unique<RecordingSelector>(database);
        recorder = selector.get();
        return selector;
    };
}

/// @brief The distinct variable counts of the clauses, ascending: a renumbering puts
/// the active ones over the variables it kept.
std::vector<std::size_t> clause_num_vars(const Solver& solver) {
    std::vector<std::size_t> counts;
    for (const Clause& clause : solver.clause_database().clauses()) {
        counts.push_back(clause.num_vars());
    }
    std::ranges::sort(counts);
    const auto repeated = std::ranges::unique(counts);
    counts.erase(repeated.begin(), repeated.end());
    return counts;
}

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
    CHECK(run.solver->stats().decisions == 0);
    CHECK(run.solver->stats().num_conflicts == 1);
    CHECK(run.solver->stats().num_propagations == 2);
    CHECK(run.solver->stats().num_restarts == 0);
    // Nothing was learned: a root conflict ends the run before analysis.
    CHECK(run.solver->num_clauses() == run.clauses.size());
}

TEST_CASE_TEMPLATE("solver: an empty input clause is UNSAT before any search", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    for (const std::string_view empty : {"0", "1+2+1+2 0", "x 0"}) {
        CAPTURE(empty);
        Run run = make_run(repr, "p cnf 2 3\n1 2 0\n" + std::string{empty} + "\n-1 0\n");

        const Solution solution = run.solver->solve();
        CHECK(solution.result == SolveResult::unsat);
        CHECK(run.solver->stats().num_conflicts == 0);
        CHECK(run.solver->stats().num_propagations == 0);
        // The empty clause never reaches the database.
        CHECK(run.solver->num_clauses() == 2);
    }
}

TEST_CASE_TEMPLATE("solver: SAT decided entirely by root propagation", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Run run = make_run(repr, unit_sat_instance);

    const Solution solution = run.solver->solve();
    check_satisfies(run.rows, solution, run.num_vars);

    // The unique solution: x1 = 1, x2 = 0, and x3 = x1 + 1 = 0.
    CHECK(solution.assignment->test(0));
    CHECK_FALSE(solution.assignment->test(1));
    CHECK_FALSE(solution.assignment->test(2));

    CHECK(run.solver->stats().num_propagations == 3);
    CHECK(run.solver->stats().num_conflicts == 0);
    CHECK(run.solver->stats().decisions == 0);
}

TEST_CASE_TEMPLATE("solver: SAT found by search", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Run run = make_run(repr, search_sat_instance);

    const Solution solution = run.solver->solve();
    check_satisfies(run.rows, solution, run.num_vars);

    // The first two clauses force exactly one of x1, x2, and no clause is unit
    // at the root, so the answer cannot be reached without deciding.
    CHECK(solution.assignment->test(0) != solution.assignment->test(1));
    CHECK(run.solver->stats().decisions > 0);
}

TEST_CASE_TEMPLATE("solver: a model is completed over what BVE eliminated", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    // x1 = x2 and x3, read by two clauses, beside clauses of equations over the rest.
    constexpr std::string_view instance = "p xnf 7 9\n"
                                          "-1 2 0\n"
                                          "-1 3 0\n"
                                          "1 -2 -3 0\n"
                                          "1 4 0\n"
                                          "-1 5 0\n"
                                          "4+5 6+7 0\n"
                                          "-4+6 2+7 0\n"
                                          "2+3+5 -6 0\n"
                                          "3+4+7 5+6 0\n";
    // The gate's hyperplane comes after more than 10 refusals in a row, and a group
    // over seven variables has fewer than 1000 hyperplanes.
    Run run = make_run(repr, instance, {"--bve=true", "--bve-max-refusals-in-a-row=1000"});

    const Solution solution = run.solver->solve();
    check_satisfies(run.rows, solution, run.num_vars);
    const RunStats stats = run.solver->stats();
    CHECK(stats.bve_eliminated > 0);
    CHECK(stats.bve_passes > 0);
}

TEST_CASE_TEMPLATE("solver: BVE does not change an UNSAT verdict", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Run run = make_run(repr, search_unsat_instance, {"--bve=true"});

    CHECK(run.solver->solve().result == SolveResult::unsat);
    const RunStats stats = run.solver->stats();
    CHECK(stats.bve_passes > 0);
}

TEST_CASE_TEMPLATE("solver: UNSAT found by search", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Run run = make_run(repr, search_unsat_instance);

    const Solution solution = run.solver->solve();
    CHECK(solution.result == SolveResult::unsat);
    CHECK_FALSE(solution.assignment.has_value());

    // Eight clauses over three variables, each ruling out one assignment: the
    // solver has to decide, conflict, and learn its way through them.
    CHECK(run.solver->stats().decisions > 0);
    CHECK(run.solver->num_clauses() > run.clauses.size());
}

TEST_CASE_TEMPLATE("solver: a clause the decision step hands back propagates with a reason", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const std::string instance = sign_pattern_instance(4);
    const std::vector<std::vector<ProofRow>> inputs =
        clause_rows(parse_dimacs_string(instance, repr).clauses);

    // Deferring after every fruitless attempt leaves unit clauses for the
    // decision step to meet.
    std::ostringstream proof;
    {
        ProofWriter writer(proof, 4);
        Run run = make_run(repr, instance,
                           {"--selection=cyclic", "--propagate-miss-rate=0.5"},
                           nullptr, &writer);
        CHECK(run.solver->solve().result == SolveResult::unsat);
    }

    // Every propagated row a chain cites has to have come with its reason.
    LrupChecker checker(repr, 4, inputs);
    CHECK(checker.check(proof.str()) == "");
}

TEST_CASE_TEMPLATE("solver: a decision a missed propagation forbids is undone", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Run run = make_run(repr, missed_unit_instance,
                       {"--selection=cyclic", "--propagate-miss-rate=0.5", "--seed=9"});

    check_satisfies(run.rows, run.solver->solve(), run.num_vars);
    CHECK(run.solver->stats().num_undone_decisions > 0);
}

TEST_CASE_TEMPLATE("solver: a budget leaves the verdicts alone", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const std::vector<std::string> budgeted =
        std::vector<std::string>{"--selection=cyclic", "--propagate-miss-rate=0.01"};

    Run unsat = make_run(repr, search_unsat_instance, budgeted);
    CHECK(unsat.solver->solve().result == SolveResult::unsat);

    Run sat = make_run(repr, compaction_sat_instance, budgeted);
    check_satisfies(sat.rows, sat.solver->solve(), sat.num_vars);
}

TEST_CASE_TEMPLATE("solver: bumping the learned clause's pivots leaves the answers alone", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const auto bumping = [](std::string_view instance) {
        return make_run(repr, instance,
                        std::vector<std::string>{"--csids-bump-learned-positions=true"});
    };

    Run unsat = bumping(search_unsat_instance);
    CHECK(unsat.solver->solve().result == SolveResult::unsat);

    Run sat = bumping(compaction_sat_instance);
    check_satisfies(sat.rows, sat.solver->solve(), sat.num_vars);
}

TEST_CASE_TEMPLATE("solver: the counters a search run accumulates", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Run run = make_run(repr, search_unsat_instance);
    CHECK(run.solver->stats().num_conflicts == 0);

    const Solution solution = run.solver->solve();
    REQUIRE(solution.result == SolveResult::unsat);

    const RunStats counters = run.solver->stats();
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
}

TEST_CASE("solver: the two representations agree on what the search did") {
    // Deliberately not a `TEST_CASE_TEMPLATE`: the claim is about the two
    // representations together.
    for (const std::string selection : {"--selection=cyclic", "--selection=exact-watchlist"}) {
        CAPTURE(selection);
        Run dense = make_run(EquationRepr::dense, search_unsat_instance, {selection});
        Run sparse = make_run(EquationRepr::sparse, search_unsat_instance, {selection});

        const Solution dense_solution = dense.solver->solve();
        const Solution sparse_solution = sparse.solver->solve();
        CHECK(dense_solution.result == sparse_solution.result);

        const RunStats dense_counters = dense.solver->stats();
        const RunStats sparse_counters = sparse.solver->stats();
        CHECK(dense_counters.num_conflicts == sparse_counters.num_conflicts);
        CHECK(dense_counters.num_propagations == sparse_counters.num_propagations);
        CHECK(dense_counters.num_attempt_props == sparse_counters.num_attempt_props);
        CHECK(dense_counters.num_watch_updates == sparse_counters.num_watch_updates);
        CHECK(dense_counters.num_restarts == sparse_counters.num_restarts);
    }
}

TEST_CASE_TEMPLATE("solver: compaction runs and the answer comes back in the original space", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    Run with = make_run(repr, compaction_sat_instance);
    const Solution with_solution = with.solver->solve();

    // The root trail determines variables 0, 1 and 3, leaving x3 and x5 to
    // search in, while `num_vars()` still reports the original space.
    CHECK(with.solver->stats().num_dropped_vars > 0);
    CHECK(with.solver->active_num_vars() < 5);
    REQUIRE(with.solver->num_vars() == 5);

    // An assignment over the *original* five variables.
    check_satisfies(with.rows, with_solution, with.num_vars);

    // The back-substituted variables, checked against the instance by hand:
    // x1 = 1 and x2 = 0 are the root units, and x4 = x5 + 1.
    CHECK(with_solution.assignment->test(0));
    CHECK_FALSE(with_solution.assignment->test(1));
    CHECK(with_solution.assignment->test(3) != with_solution.assignment->test(4));
    CHECK(with_solution.assignment->test(2) != with_solution.assignment->test(3));
}

TEST_CASE_TEMPLATE("solver: a compaction renumbers once that frees enough words", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    // Five variables and the constant fit one word whatever is dropped.
    Run narrow = make_run(repr, compaction_sat_instance);
    (void)narrow.solver->solve();
    CHECK(narrow.solver->stats().num_dropped_vars >= 3);
    CHECK(clause_num_vars(*narrow.solver) == std::vector<std::size_t>{5});
    CHECK(narrow.solver->active_num_vars() == 5 - narrow.solver->stats().num_dropped_vars);

    // 64 variables and the constant need two words, and 63 need one.
    // The fifth clause uses the variables the others do not.
    std::string wide_instance = "p cnf 64 5\n1 0\n2+64 0\n63 64 0\n-63 -64 0\n";
    for (std::size_t var = 3; var <= 62; ++var) {
        wide_instance += std::to_string(var) + " ";
    }
    wide_instance += "0\n";
    Run wide = make_run(repr, wide_instance);
    const Solution solution = wide.solver->solve();
    CHECK(clause_num_vars(*wide.solver).front() < 64);
    CHECK(wide.solver->stats().num_dropped_vars >= 2);
    CHECK(wide.solver->active_num_vars() == 64 - wide.solver->stats().num_dropped_vars);
    check_satisfies(wide.rows, solution, wide.num_vars);
    CHECK(solution.assignment->test(0));
    CHECK(solution.assignment->test(1) != solution.assignment->test(63));
    CHECK(solution.assignment->test(62) != solution.assignment->test(63));
}

TEST_CASE_TEMPLATE("solver: a model covers the unused variables", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    // `compaction_sat_instance` on variables 2, 4, 70, 131 and 133 of 140.
    const std::string instance = "p cnf 140 5\n2 0\n-4 0\n131+133 0\n70 131 0\n-70 -131 0\n";
    const std::vector<std::size_t> used_vars{1, 3, 69, 130, 132};
    REQUIRE(parse_dimacs_string(instance, repr).used_vars == used_vars);

    for (const std::string compact : {"--compact=false", "--compact=true"}) {
        CAPTURE(compact);
        Run run = make_run(repr, instance, {compact});
        CHECK(run.solver->num_vars() == 140);
        CHECK(run.solver->active_num_vars() == 5);

        const Solution solution = run.solver->solve();
        check_satisfies(run.rows, solution, 140);
        CHECK(run.solver->active_num_vars() == 5 - run.solver->stats().num_dropped_vars);
        const IndexSet& assignment = *solution.assignment;
        CHECK(assignment.test(1));
        CHECK_FALSE(assignment.test(3));
        CHECK(assignment.test(130) != assignment.test(132));
        CHECK(assignment.test(69) != assignment.test(130));
        std::size_t num_true_used = 0;
        for (const std::size_t var : used_vars) {
            num_true_used += assignment.test(var) ? 1U : 0U;
        }
        CHECK(assignment.count() == num_true_used);
    }
}

TEST_CASE_TEMPLATE("solver: a proof is over the unused variables too", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    // `search_unsat_instance` on variables 2, 5 and 8 of 9.
    const std::string instance = "p cnf 9 8\n2 5 8 0\n2 5 -8 0\n2 -5 8 0\n2 -5 -8 0\n"
                                 "-2 5 8 0\n-2 5 -8 0\n-2 -5 8 0\n-2 -5 -8 0\n";
    const std::vector<std::vector<ProofRow>> inputs =
        clause_rows(parse_dimacs_string(instance, repr));
    CHECK(inputs[1] == std::vector<ProofRow>{{1}, {4}, {7, 9}});

    std::ostringstream proof;
    {
        ProofWriter writer(proof, 9);
        Run run = make_run(repr, instance, {}, nullptr, &writer);
        CHECK(run.solver->solve().result == SolveResult::unsat);
        CHECK(run.solver->stats().num_conflicts > 0);
    }

    LrupChecker checker(repr, 9, inputs);
    CHECK(checker.check(proof.str()) == "");
}

TEST_CASE_TEMPLATE("solver: compaction does not change an UNSAT verdict", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    Run with = make_run(repr, search_unsat_instance);
    CHECK(with.solver->solve().result == SolveResult::unsat);
    CHECK(with.solver->stats().num_dropped_vars > 0);
}

TEST_CASE_TEMPLATE("solver: restarting does not change the verdict", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Run restarting = make_run(repr, search_unsat_instance, tiny_luby_restarts());

    CHECK(restarting.solver->solve().result == SolveResult::unsat);
    CHECK(restarting.solver->stats().num_restarts > 0);

    Run satisfiable = make_run(repr, search_sat_instance, tiny_luby_restarts());
    const Solution solution = satisfiable.solver->solve();
    check_satisfies(satisfiable.rows, solution, satisfiable.num_vars);
}

TEST_CASE_TEMPLATE("solver: a backjump removes the levels it says it removes", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    // `removed_levels` is what the solver discarded, which is the complement of
    // the asserting level only when no restart fired. The restarting subcase is
    // where the two quantities come apart.
    auto record = [&](const std::vector<std::string>& restarts) {
        RecordingSelector* recorder = nullptr;
        Run run = make_run(repr, search_unsat_instance, old_heuristics() + restarts,
                           recording_selector(recorder));
        CHECK(run.solver->solve().result == SolveResult::unsat);
        return recorder->backjumps;
    };

    SUBCASE("without restarts the removed count is the asserting level's complement") {
        const std::vector<Backjump> backjumps = record({});
        REQUIRE_FALSE(backjumps.empty());
        for (const Backjump& backjump : backjumps) {
            CHECK(backjump.level_after + backjump.removed_levels == backjump.level_before);
            CHECK(backjump.level_after == backjump.asserting_level);
            CHECK(backjump.removed_levels == backjump.level_before - backjump.asserting_level);
        }
    }

    SUBCASE("with restarts it is every non-root level") {
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
}

TEST_CASE_TEMPLATE("solver: the learned clause's LBD reaches the selector", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    RecordingSelector* recorder = nullptr;
    Run run = make_run(repr, search_unsat_instance, old_heuristics(), recording_selector(recorder));
    const std::size_t num_input_clauses = run.clauses.size();
    CHECK(run.solver->solve().result == SolveResult::unsat);

    const ClauseDatabase& database = run.solver->clause_database();
    const std::vector<Backjump>& backjumps = recorder->backjumps;
    REQUIRE(database.num_clauses() == num_input_clauses + backjumps.size());
    REQUIRE_FALSE(backjumps.empty());

    // Deletion is off, and a use as a reason measures a starting clause all the same.
    std::size_t num_measured = 0;
    for (ClauseId id = 0; id < num_input_clauses; ++id) {
        CHECK_FALSE(database.record(id).learned);
        num_measured += database.record(id).lbd_measured ? 1U : 0U;
    }
    CHECK(num_measured > 0);
    for (std::size_t index = 0; index < backjumps.size(); ++index) {
        const ClauseId id = num_input_clauses + index;
        const std::size_t lbd = backjumps[index].lbd;
        CHECK(database.record(id).learned);
        CHECK(database.record(id).lbd_measured);
        CHECK(backjumps[index].record_lbd == static_cast<double>(lbd));
        // The clause is derived from the levels of the conflicting trail.
        CHECK(lbd <= backjumps[index].level_before);
    }
}


// A run is due at the start and each time the conflict
// interval elapses; one with too few new width-2 clauses is skipped.
TEST_CASE_TEMPLATE("solver: what makes an implication graph run due", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    const std::string never = "1099511627776";
    const std::vector<std::string> without_graph = {
        "--selection=cyclic", "--propagate-miss-rate=0", "--deletion=false", "--otf-subsume=false",
        "--vivify=false"};

    const auto stats = [&](const std::vector<std::string>& graph) {
        Run run = make_run(repr, search_unsat_instance, without_graph + graph);
        CHECK(run.solver->solve().result == SolveResult::unsat);
        return run.solver->stats();
    };

    const RunStats idle =
        stats({"--implication-graph=true", "--implication-graph-min-new-clauses=0",
               "--implication-graph-cadence=" + never});
    CHECK(idle.implication_graph_skipped_runs == 0);
    CHECK(idle.num_forced_restarts == 0);

    const RunStats eager =
        stats({"--implication-graph=true", "--implication-graph-min-new-clauses=0",
               "--implication-graph-cadence=1"});
    CHECK(eager.implication_graph_skipped_runs == 0);
    CHECK(eager.implication_graph_runs > idle.implication_graph_runs);
    CHECK(eager.implication_graph_average_new_width_2_clauses.has_value());

    // Every due point is skipped, the first included, and none forces a restart.
    const RunStats skipping =
        stats({"--implication-graph=true", "--implication-graph-min-new-clauses=" + never,
               "--implication-graph-cadence=1"});
    CHECK(skipping.implication_graph_runs == 0);
    CHECK(skipping.implication_graph_skipped_runs > 0);
    CHECK(!skipping.implication_graph_average_new_width_2_clauses.has_value());
    CHECK(skipping.num_forced_restarts == 0);

    // Off, nothing is read however many clauses narrow.
    const RunStats off = stats({"--implication-graph=false"});
    CHECK(off.implication_graph_runs == 0);
}

// A row of the clause's linear negation that the trail already implies is not
// assumed, and the replacement is the clause of the rows assumed, so the implied
// row leaves the clause. Here `x1 = 0` is implied by propagation from any assumed
// row other than `x1 = 0` itself. A pass runs at the first root visit, its budget a
// hundredth of the root's one propagation: one order.
TEST_CASE("solver: vivification removes a row the trail implies from the clause") {
    const std::vector<std::string> flags = {"--vivify=true", "--vivify-bases=1",
                                            "--vivify-cadence=0",
                                            "--bve=false", "--implication-graph=false"};
    // (x1 v x2 v x3), and for each other nonzero v in the span of x1, x2, x3,
    // [v = 1] v [x1 = 0]. An order whose first row is not `x1 = 0` assumes it,
    // propagates `x1 = 0`, and ends with two rows that do not span `x1 = 0`;
    // one whose first row is `x1 = 0` assumes all three and narrows nothing.
    // The unit [x4 = 1] is the root's propagation.
    constexpr std::string_view instance = "p xnf 4 8\n"
                                          "4 0\n"
                                          "1 2 3 0\n"
                                          "2 -1 0\n"
                                          "3 -1 0\n"
                                          "1+2 -1 0\n"
                                          "1+3 -1 0\n"
                                          "2+3 -1 0\n"
                                          "1+2+3 -1 0\n";
    std::size_t num_narrowed = 0;
    for (std::size_t seed = 0; seed < 40; ++seed) {
        CAPTURE(seed);
        Run run = make_run(EquationRepr::dense, instance,
                           flags + std::vector<std::string>{"--seed=" + std::to_string(seed)});
        CHECK(run.solver->solve().result == SolveResult::sat);
        const RunStats stats = run.solver->stats();
        if (stats.vivify_narrowed == 0) {
            continue;
        }
        ++num_narrowed;
        CHECK(stats.vivify_narrowed == 1);
        CHECK(stats.vivify_average_width_removed == 1.0);
        // The narrowing overwrites (x1 v x2 v x3) under its own id.
        const Clause& replacement = run.solver->clause_database().clauses()[1];
        REQUIRE(replacement.rows() == 2);
        Equation x1 = make_equation_with(EquationRepr::dense, 4, {0});
        CHECK(replacement.partial_reduce(x1).kind != ReduceResult::Kind::eq_true);
    }
    // An order starts with `x1 = 0` one time in seven.
    CHECK(num_narrowed >= 25);
}

// A clause another subsumes can narrow to exactly the other's span, through that
// clause's own propagation; the kept clause stands for it and nothing is added.
TEST_CASE("solver: a vivification narrowing a kept clause already has adds no clause") {
    // (x1 v x2) subsumes (x1 v x2 v x3); the unit [x4 = 1] is the root's propagation.
    constexpr std::string_view instance = "p cnf 4 3\n"
                                          "4 0\n"
                                          "1 2 0\n"
                                          "1 2 3 0\n";
    std::size_t num_kept = 0;
    for (std::size_t seed = 0; seed < 40; ++seed) {
        CAPTURE(seed);
        Run run = make_run(EquationRepr::dense, instance,
                           {"--vivify=true", "--vivify-bases=1", "--vivify-cadence=0",
                            "--bve=false", "--implication-graph=false",
                            "--seed=" + std::to_string(seed)});
        CHECK(run.solver->solve().result == SolveResult::sat);
        const std::size_t kept = run.solver->stats().num_equivalent_clauses;
        if (kept != 0) {
            ++num_kept;
            // The unit's clause went at the substitution of x4.
            CHECK(run.solver->num_active_clauses() == 1);
            CHECK(run.solver->clause_database().num_clauses() == 3);
        }
    }
    CHECK(num_kept > 0);
}

// A tautology stays out of the database and keeps its place in the proof's
// numbering. Kept, `x1 = 0 v x1 = 1 v x2 = 1` would be the one clause wide enough
// for the pass at the first root visit, and an order assuming `x1 + x2 = 1` first
// would narrow it at `x1 + x2 = 0`, refuted by its own `0 = 1`, to a tautology no
// trail derives, as most of these seeds do. The unit [x5 = 1] is the
// root's propagation, which the pass's budget is a multiple of.
TEST_CASE("solver: a tautology at input is dropped, and the proof checks") {
    constexpr std::string_view instance = "p xnf 5 6\n"
                                          "5 0\n"
                                          "-1 1 2 0\n"
                                          "3 4 0\n"
                                          "3 -4 0\n"
                                          "-3 4 0\n"
                                          "-3 -4 0\n";
    const std::vector<std::vector<ProofRow>> inputs =
        clause_rows(parse_dimacs_string(instance, EquationRepr::dense));
    for (std::size_t seed = 0; seed < 20; ++seed) {
        CAPTURE(seed);
        std::ostringstream proof;
        {
            ProofWriter writer(proof, 5);
            Run run = make_run(EquationRepr::dense, instance,
                               {"--vivify=true", "--vivify-cadence=0", "--vivify-effort=1000",
                                "--bve=false", "--implication-graph=false",
                                "--seed=" + std::to_string(seed)},
                               nullptr, &writer);
            CHECK(run.solver->solve().result == SolveResult::unsat);
            CHECK(run.solver->stats().num_tautologies_at_input == 1);
        }
        LrupChecker checker(EquationRepr::dense, 5, inputs);
        CHECK(checker.check(proof.str()) == "");
    }
}

// A pass at the first root visit, before any decision: (-x1 v x2) strengthens
// (x1 v x2 v x3), and (x2 v x4) subsumes (x2 v x4 v x5); no other pair is related,
// and the clause the strengthening makes waits for the next pass. Its rows are
// x2 = 0 and one of x3 = 0 and x1 + x3 = 0, drawn uniformly, since either clause is
// implied by the two and narrower than the wider of them.
TEST_CASE("solver: a subsumption pass deletes and strengthens") {
    // The unit [x6 = 1] is the root's propagation, which gives the budget a row of
    // the search.
    constexpr std::string_view instance = "p cnf 6 5\n"
                                          "1 2 3 0\n"
                                          "-1 2 0\n"
                                          "2 4 0\n"
                                          "2 4 5 0\n"
                                          "6 0\n";
    const auto plain = rows_of(make_clause(EquationRepr::dense, 5, {{1}, {2}}));
    const auto merged = rows_of(make_clause(EquationRepr::dense, 5, {{0, 2}, {1}}));
    std::size_t num_plain = 0;
    std::size_t num_merged = 0;
    for (std::size_t seed = 0; seed < 40; ++seed) {
        CAPTURE(seed);
        Run run = make_run(EquationRepr::dense, instance,
                           {"--inproc-subsume-strengthen=true", "--inproc-subsume-strengthen-cadence=0",
                            "--inproc-subsume-strengthen-effort=1000000", "--implication-graph=false",
                            "--seed=" + std::to_string(seed)});
        CHECK(run.solver->solve().result == SolveResult::sat);
        CHECK(run.solver->stats().num_inproc_subsumed_clauses == 1);
        CHECK(run.solver->stats().num_inproc_strengthened_clauses == 1);
        CHECK(run.solver->num_active_clauses() == 3);
        // The strengthening overwrites (x1 v x2 v x3) under its own id.
        const auto rows = rows_of(run.solver->clause_database().clauses()[0]);
        num_plain += rows == plain ? 1U : 0U;
        num_merged += rows == merged ? 1U : 0U;
    }
    CHECK(num_plain + num_merged == 40);
    CHECK(num_plain >= 10);
    CHECK(num_merged >= 10);
}

// (-x1 v x2) and (x1 v x2) leave the unit x2, which goes through the unit path; the
// unit [x4 = 1] gives the budget a row of the search.
TEST_CASE("solver: a subsumption pass strengthening to a unit feeds it") {
    constexpr std::string_view instance = "p cnf 4 4\n"
                                          "1 2 0\n"
                                          "-1 2 0\n"
                                          "1 3 0\n"
                                          "4 0\n";
    Run run = make_run(EquationRepr::dense, instance,
                       {"--inproc-subsume-strengthen=true", "--inproc-subsume-strengthen-cadence=0",
                        "--inproc-subsume-strengthen-effort=1000000", "--implication-graph=false"});
    const Solution solution = run.solver->solve();
    REQUIRE(solution.result == SolveResult::sat);
    CHECK(solution.assignment->test(1));
    CHECK(run.solver->stats().num_inproc_subsumed_clauses == 0);
    CHECK(run.solver->stats().num_inproc_strengthened_clauses == 1);
}

} // namespace xorcle
