#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "cli/options.h"
#include "core/analysis.h"
#include "core/clause_id.h"
#include "core/solver.h"
#include "core/trail.h"
#include "heuristics/decisions/csids.h"
#include "heuristics/restarts/restart_policy.h"
#include "heuristics/selection/cyclic.h"
#include "heuristics_test_support.h"
#include "io/dimacs.h"
#include "io/proof_raw.h"
#include "io/proof_raw_file.h"
#include "util/random.h"
#include "util/stats.h"

namespace xorcle {

namespace {

/// @brief A conflicting trail together with the clause database that built it.
struct Conflict {
    Trail trail;
    std::vector<Clause> clauses; ///< The clause database, in clause-id order.
};

/// @brief The conflict of the extended example of arXiv:2605.15002, over
/// `a, b, c, d, e` numbered `x0 .. x4`, so bit 5 is the constant.
///
/// | step | equation | reason | level |
/// |---|---|---|---|
/// | 0 | `[b + c = 0]` | decision | 1 |
/// | 1 | `[a + b + d = 1]` | decision | 2 |
/// | 2 | `[d + e = 0]` | clause 0 | 2 |
/// | 3 | `[c + e = 1]` | clause 1 | 2 |
/// | 4 | `0 = 1` | clause 2 | 2 |
///
/// Every clause and every unit is already in row-echelon form, as the example
/// arranges. The paper's learned clause is as small as any clause the conflict
/// justifies, so minimization has nothing to take off it.
Conflict make_extended_example(EquationRepr repr) {
    constexpr std::size_t num_vars = 5;
    std::vector<Clause> clauses;
    // C1 = [a + b + e = 1] v [b + c + d + e = 0]
    clauses.push_back(make_clause(repr, num_vars, {{0, 1, 4}, {1, 2, 3, 4, 5}}));
    // C2 = [b + d = 1] v [d + e = 1]
    clauses.push_back(make_clause(repr, num_vars, {{1, 3}, {3, 4}}));
    // C3 = [b + e = 0] v [c + d = 0]
    clauses.push_back(make_clause(repr, num_vars, {{1, 4, 5}, {2, 3, 5}}));

    Trail trail(repr, num_vars);
    trail.learn(make_equation_with(repr, num_vars, {1, 2}), std::nullopt);
    trail.learn(make_equation_with(repr, num_vars, {0, 1, 3, 5}), std::nullopt);
    trail.learn(make_equation_with(repr, num_vars, {3, 4}), ClauseId{0});
    trail.learn(make_equation_with(repr, num_vars, {2, 4, 5}), ClauseId{1});
    trail.learn(Equation::eq_false(repr, num_vars), ClauseId{2});
    return Conflict{std::move(trail), std::move(clauses)};
}

/// @brief A conflict over two decision levels whose 1-UIP clause is not the
/// narrowest the trail justifies.
///
/// Six variables.
///
/// | step | equation | reason | level |
/// |---|---|---|---|
/// | 0 | `[x0 = 0]` | decision | 1 |
/// | 1 | `[x5 = 1]` | clause 0 | 1 |
/// | 2 | `[x1 = 0]` | decision | 2 |
/// | 3 | `[x2 = 1]` | clause 1 | 2 |
/// | 4 | `[x3 = 1]` | clause 2 | 2 |
/// | 5 | `[x4 = 1]` | clause 3 | 2 |
/// | 6 | `0 = 1` | clause 4 | 2 |
///
/// The 1-UIP clause is `[x2 = 0] v [x5 = 0] v [x0 = 1]`. Its `[x5 = 0]` is
/// there only because clause 0 propagated `[x5 = 1]` from `[x0 = 0]`, which the
/// clause already carries, so resolving that step away takes a row off.
Conflict make_shrinkable_conflict(EquationRepr repr) {
    constexpr std::size_t num_vars = 6;
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{0}, {5}})); // 0: implies [x5 = 1]
    // 1: [x1 = 1] v [x5 = 0] v [x2 = 1], propagating [x2 = 1] at level 2.
    clauses.push_back(make_clause(repr, num_vars, {{1}, {5, 6}, {2}}));
    clauses.push_back(make_clause(repr, num_vars, {{2, 6}, {3}})); // 2: implies [x3 = 1]
    clauses.push_back(make_clause(repr, num_vars, {{2, 6}, {4}})); // 3: implies [x4 = 1]
    // 4: [x3 + x4 = 1] v [x2 = 0] v [x5 = 0] v [x0 = 1], every equation falsified.
    clauses.push_back(make_clause(repr, num_vars, {{3, 4}, {2, 6}, {5, 6}, {0}}));

    Trail trail(repr, num_vars);
    trail.learn(make_equation_with(repr, num_vars, {0}), std::nullopt);
    trail.learn(make_equation_with(repr, num_vars, {5, 6}), ClauseId{0});
    trail.learn(make_equation_with(repr, num_vars, {1}), std::nullopt);
    trail.learn(make_equation_with(repr, num_vars, {2, 6}), ClauseId{1});
    trail.learn(make_equation_with(repr, num_vars, {3, 6}), ClauseId{2});
    trail.learn(make_equation_with(repr, num_vars, {4, 6}), ClauseId{3});
    trail.learn(Equation::eq_false(repr, num_vars), ClauseId{4});
    return Conflict{std::move(trail), std::move(clauses)};
}

/// @brief The rows of a clause or row space, as lists of set bits.
std::vector<std::vector<std::size_t>> rows_of(const RowEchelonMatrix& matrix) {
    std::vector<std::vector<std::size_t>> rows;
    for (const Equation& row : matrix) {
        rows.push_back(bits_of(row));
    }
    return rows;
}

/// @brief How many dimensions `clause` adds to `span`; the clause is asserting
/// exactly when this is 1. Counted as a rank, so it does not depend on which
/// basis of the clause happens to be stored.
std::size_t dimensions_outside_span(const Clause& clause, const RowSpace& span) {
    RowSpace enlarged = span;
    std::size_t added = 0;
    for (const Equation& row : clause) {
        added += enlarged.add_row_get_row_idx(row).has_value() ? 1 : 0;
    }
    return added;
}

/// @brief Whether every row of `inner` lies in `outer`'s span.
bool spans_within(const RowEchelonMatrix& inner, const RowEchelonMatrix& outer) {
    RowEchelonMatrix enclosing = outer;
    for (const Equation& row : inner) {
        if (enclosing.add_row_get_row_idx(row).has_value()) {
            return false;
        }
    }
    return true;
}

/// @brief Parse a command line, discarding the help stream.
Options parse(const std::vector<std::string>& arguments) {
    std::ostringstream help;
    const std::optional<Options> options = parse_options(arguments, help);
    REQUIRE(options.has_value());
    return *options;
}

std::unique_ptr<Csids> default_decisions(EquationRepr repr, std::size_t num_vars) {
    return std::make_unique<Csids>(repr, num_vars, default_seed);
}

/// @brief The learned clause of one conflict, with and without minimization.
struct BothWays {
    AssertingClause plain;
    AssertingClause minimized;
};

BothWays learn_both_ways(const Conflict& conflict, EquationRepr repr) {
    std::unique_ptr<Csids> plain_decisions = default_decisions(repr, conflict.trail.num_vars());
    std::unique_ptr<Csids> minimizing_decisions =
        default_decisions(repr, conflict.trail.num_vars());
    // A stream each, so both walks draw the same coins.
    Rng plain_rng = make_rng(default_seed, RngComponent::minimize_representative);
    Rng minimizing_rng = make_rng(default_seed, RngComponent::minimize_representative);
    return BothWays{find_asserting_clause(conflict.trail, conflict.clauses, *plain_decisions,
                                          plain_rng, false, true),
                    find_asserting_clause(conflict.trail, conflict.clauses, *minimizing_decisions,
                                          minimizing_rng, true, true)};
}

/// @brief The invariants minimization has to keep, whether or not it shrank the
/// clause on this conflict.
void check_minimization_invariants(const BothWays& both, const Trail& trail) {
    CHECK(both.plain.width_before_minimize == both.plain.clause.rows());
    CHECK(both.minimized.width_before_minimize == both.plain.clause.rows());

    // Narrower, and spanning less: the minimized negation lies inside the
    // negation of the clause the 1-UIP loop stopped at.
    CHECK(both.minimized.clause.rows() <= both.plain.clause.rows());
    CHECK(spans_within(both.minimized.clause, both.plain.clause));

    // Still asserting, and no later than before.
    CHECK(both.minimized.asserting_level <= both.plain.asserting_level);
    CHECK(both.minimized.asserting_level < trail.level());
    Trail backjumped = trail;
    backjumped.resize(both.minimized.asserting_level + 1);
    CHECK(dimensions_outside_span(both.minimized.clause, backjumped.span()) == 1);

    // The derivation is still a chain, growing only where a resolution was
    // taken, and its first entry is still the conflicting clause.
    CHECK(both.minimized.reason_ids.size() >= both.plain.reason_ids.size());
    CHECK(both.minimized.reason_ids.front() == both.plain.reason_ids.front());

    // Minimization's accepted resolutions are measured like the 1-UIP loop's.
    CHECK(both.plain.reason_lbds.size() == both.plain.reason_ids.size());
    CHECK(both.minimized.reason_lbds.size() == both.minimized.reason_ids.size());
}

/// @brief All sixteen sign patterns over four variables: UNSAT, and reached
/// through several conflicts whose learned clauses run from dimension 1 to 3.
constexpr std::string_view sign_pattern_instance = "p cnf 4 16\n"
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

struct Outcome {
    SolveResult result = SolveResult::unsat;
    SolverCounters counters;
};

Outcome solve_with(EquationRepr repr, std::string_view instance, bool minimize,
                   RawProofWriter* proof_writer = nullptr) {
    ParsedDimacs parsed = parse_dimacs_string(instance, repr);
    Solver solver(repr, parsed.num_vars, std::make_unique<Cyclic>(parsed.num_vars),
                  default_decisions(repr, parsed.num_vars), std::make_unique<NoRestarts>(),
                  proof_writer, CompactionConfig{.enabled = false}, ProgressConfig{}, minimize);
    for (Clause& clause : parsed.clauses) {
        solver.add_starting_clause(std::move(clause));
    }
    const Solution solution = solver.solve();
    return Outcome{solution.result, solver.counters()};
}

} // namespace

TEST_CASE_TEMPLATE("minimize: the extended example's clause is already minimal", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Conflict conflict = make_extended_example(repr);
    const BothWays both = learn_both_ways(conflict, repr);

    // C = [d + e = 1] v [b + c = 1], whose linear negation is spanned by
    // [b + c = 0] and [d + e = 0].
    CHECK(both.plain.clause.rows() == 2);
    CHECK(both.plain.asserting_level == 1);

    CHECK(rows_of(both.minimized.clause) == rows_of(both.plain.clause));
    CHECK(both.minimized.asserting_level == both.plain.asserting_level);
    CHECK(both.minimized.lbd == both.plain.lbd);
    check_minimization_invariants(both, conflict.trail);
}

TEST_CASE_TEMPLATE("minimize: a clause the walk can shrink", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Conflict conflict = make_shrinkable_conflict(repr);
    const BothWays both = learn_both_ways(conflict, repr);

    // The 1-UIP clause is [x2 = 0] v [x5 = 0] v [x0 = 1], negated as
    // [x0 = 0], [x2 = 1], [x5 = 1].
    CHECK(rows_of(both.plain.clause) ==
          std::vector<std::vector<std::size_t>>{{0}, {2, 6}, {5, 6}});
    CHECK(both.plain.asserting_level == 1);
    CHECK(both.plain.reason_ids == std::vector<ClauseId>{4, 3, 2});

    // Resolving [x5 = 1] away against clause 0 drops it: the only equation the
    // reason brings in is [x0 = 0], which the clause already holds. The step's
    // coin then took the popped row into the survivor, so {2, 6} reads {2, 5}.
    CHECK(rows_of(both.minimized.clause) == std::vector<std::vector<std::size_t>>{{0}, {2, 5}});
    CHECK(both.minimized.asserting_level == 1);
    CHECK(both.minimized.reason_ids == std::vector<ClauseId>{4, 3, 2, 0});
    check_minimization_invariants(both, conflict.trail);

    // Clause 0 propagated [x5 = 1] at level 1 and brings in only [x0 = 0], also
    // at level 1, so it reaches one level although the conflict is at level 2.
    CHECK(both.plain.reason_lbds == std::vector<std::size_t>{2, 1, 1});
    CHECK(both.minimized.reason_lbds == std::vector<std::size_t>{2, 1, 1, 1});
}

TEST_CASE_TEMPLATE("minimize: the parameter defaults to on", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    for (const Conflict& conflict :
         {make_extended_example(repr), make_shrinkable_conflict(repr)}) {
        std::unique_ptr<Csids> defaulted = default_decisions(repr, conflict.trail.num_vars());
        std::unique_ptr<Csids> explicitly_on = default_decisions(repr, conflict.trail.num_vars());

        Rng implicit_rng = make_rng(default_seed, RngComponent::minimize_representative);
        Rng explicit_rng = make_rng(default_seed, RngComponent::minimize_representative);
        const AssertingClause implicit =
            find_asserting_clause(conflict.trail, conflict.clauses, *defaulted, implicit_rng);
        const AssertingClause explicit_on = find_asserting_clause(
            conflict.trail, conflict.clauses, *explicitly_on, explicit_rng, true);

        CHECK(rows_of(implicit.clause) == rows_of(explicit_on.clause));
        CHECK(implicit.asserting_level == explicit_on.asserting_level);
        CHECK(implicit.lbd == explicit_on.lbd);
        CHECK(implicit.reason_ids == explicit_on.reason_ids);
    }
}

TEST_CASE_TEMPLATE("minimize: a whole run reaches the same verdict and narrower clauses", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Outcome off = solve_with(repr, sign_pattern_instance, false);
    const Outcome on = solve_with(repr, sign_pattern_instance, true);

    CHECK(off.result == SolveResult::unsat);
    CHECK(on.result == SolveResult::unsat);
    CHECK(off.counters.num_conflicts > 1);

    // Off, the counters stay untouched.
    CHECK(off.counters.num_minimized_clauses == 0);
    CHECK(off.counters.minimize_width_before == 0);
    CHECK(off.counters.minimize_width_after == 0);

    // On, every non-root conflict is counted, and the total width can only
    // have fallen.
    CHECK(on.counters.minimize_width_before > 0);
    CHECK(on.counters.minimize_width_after <= on.counters.minimize_width_before);
    CHECK(on.counters.num_minimized_clauses <= on.counters.num_conflicts);
    CHECK((on.counters.num_minimized_clauses > 0) ==
          (on.counters.minimize_width_after < on.counters.minimize_width_before));
}

TEST_CASE("minimize: the option is on by default and round-trips") {
    CHECK(parse({"instance.xnf"}).minimize);
    CHECK(parse({"--minimize=true", "instance.xnf"}).minimize);
    CHECK_FALSE(parse({"--minimize=false", "instance.xnf"}).minimize);
}

TEST_CASE("minimize: --minimize and --proof go together") {
    const Options options = parse({"--minimize=true", "--proof=out.lrup", "instance.xnf"});
    CHECK(options.minimize);
    CHECK(options.proof == "out.lrup");
}

TEST_CASE_TEMPLATE("minimize: a minimizing run still writes a raw proof", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        (repr == EquationRepr::dense ? "xorcle_minimize_proof_dense.raw"
                                     : "xorcle_minimize_proof_sparse.raw");

    {
        RawProofWriter writer(path, 4, 16);
        const Outcome outcome = solve_with(repr, sign_pattern_instance, true, &writer);
        REQUIRE(outcome.result == SolveResult::unsat);
        writer.finalize(true);
    }

    // Learned clauses first, then exactly one empty clause, last. A root unit's
    // line may cite its reason clause alone.
    RawProofFileSource source(path);
    RawRecord record;
    std::size_t learned = 0;
    std::size_t empty = 0;
    while (source.next(record)) {
        if (const RawLearned* line = std::get_if<RawLearned>(&record)) {
            CHECK(line->rows.size() > 0);
            CHECK(line->reasons.size() >= (line->rows.size() == 1 ? 1u : 2u));
            CHECK(empty == 0);
            ++learned;
        }
        if (const RawEmpty* line = std::get_if<RawEmpty>(&record)) {
            CHECK(line->reasons.size() >= 1);
            ++empty;
        }
    }
    CHECK(learned > 0);
    CHECK(empty == 1);

    std::filesystem::remove(path);
}

} // namespace xorcle
