#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "cli/options.h"
#include "core/solver.h"
#include "heuristics/implication_graph/implication_graph.h"
#include "algebra_test_support.h"
#include "heuristics_test_support.h"
#include "io/dimacs.h"
#include "io/proof_raw.h"
#include "io/proof_raw_file.h"
#include "io_test_support.h"

namespace xorcle {

namespace {

constexpr EquationRepr repr = EquationRepr::dense;

/// @brief The ids of a clause store, with the flags saying which are active.
struct StoreIds {
    std::vector<ClauseId> ids;
    std::vector<std::uint8_t> is_active;

    operator ActiveClauseIds() const { return {ids, is_active}; }
};

/// @brief The ids of a store in which every clause is active.
StoreIds all_active(const std::vector<Clause>& clauses) {
    StoreIds out{std::vector<ClauseId>(clauses.size()),
                 std::vector<std::uint8_t>(clauses.size(), 1)};
    std::iota(out.ids.begin(), out.ids.end(), ClauseId{0});
    return out;
}

/// @brief The span of @p units, so a test can name a subspace rather than the
/// basis the rule happens to hand back.
RowSpace span_of(std::size_t num_vars, const std::vector<Equation>& units) {
    RowSpace span(repr, num_vars);
    for (const Equation& unit : units) {
        span.add_row(unit);
    }
    return span;
}

bool holds(const RowSpace& span, std::size_t num_vars, const std::vector<std::size_t>& bits) {
    Equation eqn = Equation::from_bits(repr, num_vars, bits);
    span.reduce(eqn);
    return eqn.is_eq_true();
}

/// @brief The step proving each unit of @p record's run, in the run's order.
std::vector<const ImplicationGraphProofStep*> fed_steps(
    const ImplicationGraphProofRecord& record) {
    std::vector<const ImplicationGraphProofStep*> fed;
    for (const std::size_t step_idx : record.unit_idx_to_step_idx) {
        fed.push_back(&record.steps[step_idx]);
    }
    return fed;
}

} // namespace

// Variables x3, x4 and x5 are bits 2, 3 and 4; bit 5 is the constant. A clause
// stored as `[x3 = 1], [x4 = 0]` is `[x3 = 0] v [x4 = 1]`, which implies x4 from
// x3, so the three clauses below close the cycle x3 -> x4 -> x5 -> x3.
TEST_CASE("implication graph: a cycle makes its equations equivalent") {
    const std::size_t num_vars = 5;
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{2, 5}, {3}}));
    clauses.push_back(make_clause(repr, num_vars, {{3, 5}, {4}}));
    clauses.push_back(make_clause(repr, num_vars, {{4, 5}, {2}}));

    ImplicationGraph graph(repr, num_vars);
    const ImplicationGraphRun summary =
        graph.run(clauses, all_active(clauses), RowSpace(repr, num_vars));
    const std::vector<Equation>& units = summary.units;

    CHECK(summary.did_work);
    CHECK(graph.num_width_2_clauses() == 3);
    // The three equations of each clause, of which x3, x4 and x5 are shared.
    CHECK(graph.num_vertex_pairs() == 6);
    // `[x3 = 1]` spans x3, x4 and x5; its negation spans them homogeneously, so
    // the two meet in the even sums x3 + x4, x4 + x5 and x3 + x5.
    REQUIRE(units.size() == 2);
    const RowSpace found = span_of(num_vars, units);
    CHECK(found.rows() == 2);
    CHECK(holds(found, num_vars, {2, 3}));
    CHECK(holds(found, num_vars, {3, 4}));
    CHECK(holds(found, num_vars, {2, 4}));
    CHECK(!holds(found, num_vars, {2}));
}

// x1 implies x2, x2 implies x3, and x3 refutes x1, so the descendants of
// `[x1 = 1]` hold both `[x3 = 1]` and `[x3 = 0]`.
TEST_CASE("implication graph: an equation whose descendants span 0 = 1 is refuted") {
    const std::size_t num_vars = 3;
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{0, 3}, {1}}));
    clauses.push_back(make_clause(repr, num_vars, {{1, 3}, {2}}));
    clauses.push_back(make_clause(repr, num_vars, {{2, 3}, {0, 3}}));

    ImplicationGraph graph(repr, num_vars);
    const ImplicationGraphRun summary =
        graph.run(clauses, all_active(clauses), RowSpace(repr, num_vars));
    const std::vector<Equation>& units = summary.units;

    CHECK(summary.edges_visited > 0);
    CHECK(graph.num_width_2_clauses() == 3);
    CHECK(graph.num_vertex_pairs() == 6);
    // `[x1 = 0]` has no out-arc, so that one equation is the whole of its side.
    CHECK(summary.refuted_sides == 1);
    REQUIRE(units.size() == 1);
    const std::vector<std::size_t> x1_is_zero{0};
    CHECK(units[0] == Equation::from_bits(repr, num_vars, x1_is_zero));
}

// With a = [x1 = 1], b = [x2 = 1], c = [x3 = 1] and d = b + c = [x2 + x3 = 0],
// the clauses a v b, a v c and a v d spell out a -> b, a -> c and ~a -> d. Then
// d is in the span of a's descendants and in the span of ~a's, so it holds; no
// equation here reaches its own negation and no two descendants of a side are
// negations of each other. Of the seven pairs only a, ~a has an out-arc on both
// sides; ~d is refuted by a span, since d is the sum of the b and c it reaches.
TEST_CASE("implication graph: an equation can hold on both sides without a path") {
    const std::size_t num_vars = 3;
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{0, 3}, {1}}));
    clauses.push_back(make_clause(repr, num_vars, {{0, 3}, {2}}));
    clauses.push_back(make_clause(repr, num_vars, {{0}, {1, 2, 3}}));
    const std::vector<std::size_t> d{1, 2};

    ImplicationGraph graph(repr, num_vars);
    const ImplicationGraphRun summary =
        graph.run(clauses, all_active(clauses), RowSpace(repr, num_vars));
    const std::vector<Equation>& units = summary.units;

    REQUIRE(units.size() == 1);
    CHECK(units[0] == Equation::from_bits(repr, num_vars, d));
    CHECK(summary.pairs_examined == 7);
    CHECK(summary.refuted_sides == 1);
}

// a = [x1 = 1] implies [x2 = 0], [x3 = 0] and [x2 + x3 = 1], whose sum is
// `0 = 1`, so a cannot hold. No two of the three are negations of each other, so
// a reaches no equation together with its negation and never reaches ~a. ~a has
// no out-arc, so its own span is just ~a, and the intersection is exactly that.
TEST_CASE("implication graph: a span refutes an equation no pair of descendants does") {
    const std::size_t num_vars = 3;
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{0, 3}, {1, 3}}));
    clauses.push_back(make_clause(repr, num_vars, {{0, 3}, {2, 3}}));
    clauses.push_back(make_clause(repr, num_vars, {{0, 3}, {1, 2}}));
    const std::vector<std::size_t> x1_is_zero{0};

    ImplicationGraph graph(repr, num_vars);
    const ImplicationGraphRun summary =
        graph.run(clauses, all_active(clauses), RowSpace(repr, num_vars));
    const std::vector<Equation>& units = summary.units;

    REQUIRE(units.size() == 1);
    CHECK(units[0] == Equation::from_bits(repr, num_vars, x1_is_zero));
    CHECK(summary.refuted_sides == 1);
}

TEST_CASE("implication graph: a refuted root leaves nothing to analyse") {
    const std::size_t num_vars = 2;
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{0, 2}, {1}}));

    RowSpace root_span(repr, num_vars);
    root_span.add_row(Equation::eq_false(repr, num_vars));
    ImplicationGraph graph(repr, num_vars);
    const ImplicationGraphRun summary = graph.run(clauses, all_active(clauses), root_span);
    const std::vector<Equation>& units = summary.units;

    CHECK(!summary.did_work);
    CHECK(graph.num_vertex_pairs() == 0);
    CHECK(units.empty());
}

TEST_CASE("implication graph: a clause of another width contributes nothing") {
    const std::size_t num_vars = 3;
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{0, 3}}));
    clauses.push_back(make_clause(repr, num_vars, {{0, 3}, {1}, {2}}));

    ImplicationGraph graph(repr, num_vars);
    const ImplicationGraphRun summary =
        graph.run(clauses, all_active(clauses), RowSpace(repr, num_vars));

    CHECK(graph.num_width_2_clauses() == 0);
    CHECK(graph.num_vertex_pairs() == 0);
    CHECK(summary.units.empty());
}

TEST_CASE("implication graph: a unit the root trail already holds is not offered") {
    const std::size_t num_vars = 3;
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{0, 3}, {1}}));
    clauses.push_back(make_clause(repr, num_vars, {{1, 3}, {2}}));
    clauses.push_back(make_clause(repr, num_vars, {{2, 3}, {0, 3}}));

    RowSpace root_span(repr, num_vars);
    const std::vector<std::size_t> x1_is_zero{0};
    root_span.add_row(Equation::from_bits(repr, num_vars, x1_is_zero));
    ImplicationGraph graph(repr, num_vars);
    CHECK(graph.run(clauses, all_active(clauses), root_span).units.empty());
}

// The first two clauses of the cycle above leave an acyclic graph; the third
// arrives afterwards, and a run reads the database as it then stands.
TEST_CASE("implication graph: a run reads the clause database afresh") {
    const std::size_t num_vars = 5;
    const RowSpace root_span(repr, num_vars);
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{2, 5}, {3}}));
    clauses.push_back(make_clause(repr, num_vars, {{3, 5}, {4}}));

    ImplicationGraph graph(repr, num_vars);
    const ImplicationGraphRun first = graph.run(clauses, all_active(clauses), root_span);
    CHECK(first.did_work);
    REQUIRE(first.units.empty());

    clauses.push_back(make_clause(repr, num_vars, {{4, 5}, {2}}));
    const ImplicationGraphRun summary = graph.run(clauses, all_active(clauses), root_span);
    const std::vector<Equation>& units = summary.units;

    CHECK(summary.did_work);
    CHECK(graph.num_width_2_clauses() == 3);
    CHECK(units.size() == 2);
}

// Any subset of the pairs and any subset of a side's descendants leave a
// subspace of what the full walk intersects, so a capped run finds part of what
// an uncapped one does and nothing besides.
TEST_CASE("implication graph: the caps bound one run and keep its units sound") {
    const std::size_t num_vars = 5;
    const RowSpace root_span(repr, num_vars);
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{2, 5}, {3}}));
    clauses.push_back(make_clause(repr, num_vars, {{3, 5}, {4}}));
    clauses.push_back(make_clause(repr, num_vars, {{4, 5}, {2}}));

    ImplicationGraph uncapped(repr, num_vars);
    const ImplicationGraphRun full = uncapped.run(clauses, all_active(clauses), root_span);
    const std::vector<Equation>& all_units = full.units;
    REQUIRE(full.pairs_eligible > 2);
    CHECK(full.pairs_examined == full.pairs_eligible);
    CHECK(!full.at_the_pair_cap);
    CHECK(full.truncated_sides == 0);
    const RowSpace implied = span_of(num_vars, all_units);

    SUBCASE("the pair cap stops the walk at the pairs it sampled") {
        ImplicationGraph graph(repr, num_vars, ImplicationGraphConfig{.max_pairs = 2});
        const ImplicationGraphRun capped = graph.run(clauses, all_active(clauses), root_span);
        const std::vector<Equation>& units = capped.units;

        CHECK(capped.pairs_eligible == full.pairs_eligible);
        CHECK(capped.pairs_examined == 2);
        CHECK(capped.at_the_pair_cap);
        CHECK(units.size() <= all_units.size());
        for (const Equation& unit : units) {
            CHECK(holds(implied, num_vars, unit.bits()));
        }
    }

    SUBCASE("the descendant cap cuts a distance level short") {
        // Room for a side's own equation and one of the two its clause reaches.
        ImplicationGraph graph(repr, num_vars, ImplicationGraphConfig{.max_descendants = 2});
        const ImplicationGraphRun capped = graph.run(clauses, all_active(clauses), root_span);
        const std::vector<Equation>& units = capped.units;

        CHECK(capped.pairs_examined == full.pairs_eligible);
        CHECK(capped.truncated_sides > 0);
        CHECK(units.size() <= all_units.size());
        for (const Equation& unit : units) {
            CHECK(holds(implied, num_vars, unit.bits()));
        }
    }
}

// x1 -> x2 and x4 -> x5 share no variable and yield nothing, so the first run
// examines their six pairs and the second, with no clause read since, none.
TEST_CASE("implication graph: a pair reaching no clause read since it was examined is clean") {
    const std::size_t num_vars = 6;
    const RowSpace root_span(repr, num_vars);
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{0, 6}, {1}}));
    clauses.push_back(make_clause(repr, num_vars, {{3, 6}, {4}}));

    ImplicationGraph graph(repr, num_vars);
    const ImplicationGraphRun first = graph.run(clauses, all_active(clauses), root_span);
    CHECK(first.pairs_eligible == 6);
    CHECK(first.pairs_examined == 6);
    REQUIRE(first.units.empty());

    const ImplicationGraphRun second = graph.run(clauses, all_active(clauses), root_span);
    CHECK(second.pairs_eligible == 6);
    CHECK(second.pairs_examined == 0);
    CHECK(second.edges_visited == 0);

    SUBCASE("a new clause makes dirty the pairs that reach it and no other") {
        // x2 -> x3 is reached from the three pairs of x1 -> x2 and its own.
        clauses.push_back(make_clause(repr, num_vars, {{1, 6}, {2}}));
        const ImplicationGraphRun third = graph.run(clauses, all_active(clauses), root_span);
        CHECK(third.pairs_eligible == 8);
        CHECK(third.pairs_examined == 5);
        CHECK(third.units.empty());
        CHECK(graph.run(clauses, all_active(clauses), root_span).pairs_examined == 0);
    }

    SUBCASE("a new variable numbering keeps the pairs clean") {
        graph.reconfigure(num_vars);
        CHECK(graph.run(clauses, all_active(clauses), root_span).pairs_examined == 0);
    }

    SUBCASE("a deleted clause makes no pair dirty") {
        StoreIds ids = all_active(clauses);
        ids.is_active[1] = 0;
        const ImplicationGraphRun third = graph.run(clauses, ids, root_span);
        CHECK(third.pairs_eligible == 3);
        CHECK(third.pairs_examined == 0);
    }
}

// x1 -> x2, x2 -> x3 and x5 -> x6 yield nothing. Rewriting the last into
// `x3 -> not x1` under its id refutes `[x1 = 1]`, which only a run told of the
// rewrite looks for; the pairs of a fourth clause over x7 and x8 stay clean.
TEST_CASE("implication graph: a rewritten clause makes the pairs that reach it dirty") {
    const std::size_t num_vars = 8;
    const RowSpace root_span(repr, num_vars);
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{0, 8}, {1}}));
    clauses.push_back(make_clause(repr, num_vars, {{1, 8}, {2}}));
    clauses.push_back(make_clause(repr, num_vars, {{4, 8}, {5}}));
    clauses.push_back(make_clause(repr, num_vars, {{6, 8}, {7}}));

    ImplicationGraph graph(repr, num_vars);
    const ImplicationGraphRun first = graph.run(clauses, all_active(clauses), root_span);
    CHECK(first.pairs_examined == first.pairs_eligible);
    REQUIRE(first.units.empty());

    clauses[2] = make_clause(repr, num_vars, {{2, 8}, {0, 8}});
    graph.clause_was_rewritten(2);
    const ImplicationGraphRun second = graph.run(clauses, all_active(clauses), root_span);
    CHECK(second.pairs_examined == second.pairs_eligible - 3);
    const RowSpace found = span_of(num_vars, second.units);
    CHECK(holds(found, num_vars, {0}));
    CHECK(!holds(found, num_vars, {6}));
}

// x2 -> x3 and x1 -> x2 yield nothing. `x3 -> not x2` then refutes `[x2 = 1]`
// at the first pair of the second run, and `[x1 = 1]`, whose pair comes later,
// is refuted through its arc to it.
TEST_CASE("implication graph: a dirty pair is refuted through a vertex refuted in the same run") {
    const std::size_t num_vars = 6;
    const RowSpace root_span(repr, num_vars);
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{1, 6}, {2}}));
    clauses.push_back(make_clause(repr, num_vars, {{0, 6}, {1}}));
    clauses.push_back(make_clause(repr, num_vars, {{3, 6}, {4}}));

    ImplicationGraph graph(repr, num_vars);
    ImplicationGraphProofRecord record;
    const ImplicationGraphRun first =
        graph.run(clauses, all_active(clauses), root_span, &record);
    CHECK(first.pairs_examined == first.pairs_eligible);
    REQUIRE(first.units.empty());

    clauses.push_back(make_clause(repr, num_vars, {{2, 6}, {1, 6}}));
    const ImplicationGraphRun second =
        graph.run(clauses, all_active(clauses), root_span, &record);
    // The three pairs of x4 -> x5 reach nothing new.
    CHECK(second.pairs_examined == second.pairs_eligible - 3);
    const RowSpace found = span_of(num_vars, second.units);
    CHECK(holds(found, num_vars, {0}));
    CHECK(holds(found, num_vars, {1}));
    const std::vector<std::size_t> x1_is_zero{0};
    CHECK(std::ranges::any_of(record.steps, [&](const ImplicationGraphProofStep& step) {
        return step.kind == ImplicationGraphProofStep::Kind::reaches_refuted &&
               step.equation == Equation::from_bits(repr, num_vars, x1_is_zero);
    }));
}

// The refuted `[x1 = 1]` above. Its unit is found once; left as a width-1
// clause that nothing has propagated, it is not found again, the pairs being clean.
TEST_CASE("implication graph: a unit still a width-1 clause is not offered again") {
    const std::size_t num_vars = 3;
    const RowSpace root_span(repr, num_vars);
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{0, 3}, {1}}));
    clauses.push_back(make_clause(repr, num_vars, {{1, 3}, {2}}));
    clauses.push_back(make_clause(repr, num_vars, {{2, 3}, {0, 3}}));

    ImplicationGraph graph(repr, num_vars);
    REQUIRE(graph.run(clauses, all_active(clauses), root_span).units.size() == 1);

    clauses.push_back(make_clause(repr, num_vars, {{0, 3}}));
    const ImplicationGraphRun second = graph.run(clauses, all_active(clauses), root_span);
    CHECK(second.pairs_examined == 0);
    CHECK(second.units.empty());
}

// Nine pairs with nothing to find and room for two a run: the pairs a run did
// not draw stay dirty, so five runs examine each pair once and a sixth none.
TEST_CASE("implication graph: a pair the pair cap left out stays dirty") {
    const std::size_t num_vars = 6;
    const RowSpace root_span(repr, num_vars);
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{0, 6}, {1}}));
    clauses.push_back(make_clause(repr, num_vars, {{2, 6}, {3}}));
    clauses.push_back(make_clause(repr, num_vars, {{4, 6}, {5}}));

    ImplicationGraph graph(repr, num_vars, ImplicationGraphConfig{.max_pairs = 2});
    for (const std::size_t expected : {2U, 2U, 2U, 2U, 1U, 0U}) {
        const ImplicationGraphRun summary = graph.run(clauses, all_active(clauses), root_span);
        CHECK(summary.pairs_eligible == 9);
        CHECK(summary.pairs_examined == expected);
        CHECK(summary.at_the_pair_cap == (expected == 2));
    }
}

// The refuted side above, read again for what a proof of its unit would replay.
TEST_CASE("implication graph: a refuted side records the arcs it was refuted through") {
    const std::size_t num_vars = 3;
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{0, 3}, {1, 3}}));
    clauses.push_back(make_clause(repr, num_vars, {{0, 3}, {2, 3}}));
    clauses.push_back(make_clause(repr, num_vars, {{0, 3}, {1, 2}}));

    ImplicationGraph graph(repr, num_vars);
    ImplicationGraphProofRecord record;
    const std::vector<Equation> units =
        graph.run(clauses, all_active(clauses), RowSpace(repr, num_vars), &record).units;

    REQUIRE(units.size() == 1);
    const std::vector<const ImplicationGraphProofStep*> fed = fed_steps(record);
    REQUIRE(fed.size() == 1);
    CHECK(fed[0]->kind == ImplicationGraphProofStep::Kind::refuted);
    CHECK(fed[0]->equation == units[0]);
    CHECK(fed[0]->arcs_end > fed[0]->arcs_begin);
    for (std::size_t i = fed[0]->arcs_begin; i < fed[0]->arcs_end; ++i) {
        CHECK(record.arcs[i] < clauses.size());
    }
}

// The pair with an out-arc on both sides comes first, so its unit is the one
// fed; the pair of that unit and its negation is read afterwards and refutes the
// negation, which is a step the record keeps and no unit.
TEST_CASE("implication graph: a unit on both sides records a replay per side") {
    const std::size_t num_vars = 3;
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{0, 3}, {1}}));
    clauses.push_back(make_clause(repr, num_vars, {{0, 3}, {2}}));
    clauses.push_back(make_clause(repr, num_vars, {{0}, {1, 2, 3}}));

    ImplicationGraph graph(repr, num_vars);
    ImplicationGraphProofRecord record;
    const std::vector<Equation> units =
        graph.run(clauses, all_active(clauses), RowSpace(repr, num_vars), &record).units;

    REQUIRE(units.size() == 1);
    const std::vector<const ImplicationGraphProofStep*> fed = fed_steps(record);
    REQUIRE(fed.size() == 1);
    CHECK(fed[0]->kind == ImplicationGraphProofStep::Kind::both_sides);
    CHECK(fed[0]->equation == units[0]);
    CHECK(fed[0]->arcs_end > fed[0]->arcs_begin);
    CHECK(fed[0]->other_arcs_end > fed[0]->other_arcs_begin);
    CHECK(record.steps.size() > fed.size());
}

TEST_CASE("implication graph: a clause over other variables adds a component and no unit") {
    const std::size_t num_vars = 5;
    const RowSpace root_span(repr, num_vars);
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{0, 5}, {1}}));
    clauses.push_back(make_clause(repr, num_vars, {{1, 5}, {2}}));
    clauses.push_back(make_clause(repr, num_vars, {{3, 5}, {4}}));

    ImplicationGraph graph(repr, num_vars);
    const ImplicationGraphRun summary = graph.run(clauses, all_active(clauses), root_span);
    const std::vector<Equation>& units = summary.units;

    CHECK(summary.did_work);
    CHECK(graph.num_width_2_clauses() == 3);
    CHECK(units.empty());
}


namespace {

/// @brief A random database of mostly width-2 clauses over @p num_vars
/// variables, each equation over one or two variables.
std::vector<Clause> random_width_2_clauses(TestRandom& random, std::size_t num_vars) {
    std::vector<Clause> clauses;
    const std::size_t num_clauses = num_vars + random.next(2 * num_vars);
    for (std::size_t index = 0; index < num_clauses; ++index) {
        Clause clause(repr, num_vars);
        const std::size_t width = random.chance(6) ? 3 : 2;
        while (clause.rows() < width) {
            std::set<std::size_t> bits{random.next(num_vars)};
            if (random.chance(3)) {
                bits.insert(random.next(num_vars));
            }
            if (random.chance(2)) {
                bits.insert(num_vars);
            }
            const std::vector<std::size_t> sorted(bits.begin(), bits.end());
            const Equation row = Equation::from_bits(repr, num_vars, sorted);
            if (!row.is_eq_false()) {
                clause.add_row(row);
            }
        }
        clauses.push_back(std::move(clause));
    }
    return clauses;
}

/// @brief Whether @p eqn holds in every assignment satisfying the active clauses.
bool holds_in_every_model(const Equation& eqn, const std::vector<Clause>& clauses,
                          const std::vector<std::uint8_t>& active, std::size_t num_vars) {
    const auto value = [&](const Equation& row, std::uint64_t assignment) {
        bool parity = false;
        for (const std::size_t bit : row.bits()) {
            parity ^= bit == num_vars || ((assignment >> bit) & 1U) != 0;
        }
        return !parity;
    };
    for (std::uint64_t assignment = 0; assignment < (std::uint64_t{1} << num_vars);
         ++assignment) {
        bool model = true;
        for (std::size_t id = 0; id < clauses.size() && model; ++id) {
            if (active[id] == 0) {
                continue;
            }
            // A clause holds where one of its rows, its equations negated, fails.
            bool satisfied = false;
            for (const Equation& row : clauses[id]) {
                satisfied = satisfied || !value(row, assignment);
            }
            model = satisfied;
        }
        if (model && !value(eqn, assignment)) {
            return false;
        }
    }
    return true;
}

} // namespace

// Any sound rule may only ever offer what every model of the active clauses
// satisfies, whatever the caps leave it to read.
TEST_CASE("implication graph: every unit holds in every model of the active clauses") {
    TestRandom random(8675309);
    std::set<ImplicationGraphProofStep::Kind> kinds;
    std::size_t num_units = 0;
    for (std::size_t trial = 0; trial < 100; ++trial) {
        const std::size_t num_vars = 3 + random.next(6);
        const std::vector<Clause> clauses = random_width_2_clauses(random, num_vars);
        std::vector<std::uint8_t> active(clauses.size(), 1);
        for (std::uint8_t& flag : active) {
            flag = random.chance(5) ? 0 : 1;
        }
        CAPTURE(trial);

        for (const ImplicationGraphConfig& config :
             {ImplicationGraphConfig{},
              ImplicationGraphConfig{.max_pairs = 1 + random.next(4),
                                     .max_descendants = 1 + random.next(3)}}) {
            ImplicationGraph graph(repr, num_vars, config);
            graph.set_base_seed(trial);
            ImplicationGraphProofRecord record;
            const ImplicationGraphRun run =
                graph.run(clauses, StoreIds{all_active(clauses).ids, active},
                          RowSpace(repr, num_vars), &record);
            for (const Equation& unit : run.units) {
                CHECK(holds_in_every_model(unit, clauses, active, num_vars));
            }
            num_units += run.units.size();
            const std::vector<const ImplicationGraphProofStep*> fed = fed_steps(record);
            REQUIRE(fed.size() == run.units.size());
            for (std::size_t i = 0; i < fed.size(); ++i) {
                CHECK(fed[i]->equation == run.units[i]);
                kinds.insert(fed[i]->kind);
            }
        }
    }
    CHECK(num_units > 0);
    CHECK(kinds.size() == 4);
}

namespace {

/// @brief A raw row as the proof spells it: `-` for a set constant bit, then
/// the 1-based variables joined by `+`.
std::string proof_token(const RawRow& row, std::size_t num_vars) {
    std::string token = !row.empty() && row.back() == num_vars ? "-" : "";
    for (const std::size_t bit : row) {
        if (bit != num_vars) {
            token += (token.empty() || token == "-" ? "" : "+") + std::to_string(bit + 1);
        }
    }
    return token;
}

std::string proof_ids(const std::vector<ClauseId>& ids) {
    std::string text;
    for (const ClauseId id : ids) {
        text += " " + std::to_string(id + 1);
    }
    return text;
}

/// @brief The raw proof at @p path as LRUP(⊕) lines, up to the first
/// compaction that eliminates units: before it, raw ids are proof ids less one.
std::vector<std::string> raw_lines_before_compaction(const std::filesystem::path& path,
                                                     std::size_t num_vars) {
    std::vector<std::string> lines;
    RawProofFileSource source(path);
    while (const std::optional<RawRecord> next = source.next()) {
        if (const auto* compaction = std::get_if<RawCompaction>(&*next)) {
            if (!compaction->units.empty()) {
                break;
            }
        } else if (const auto* learned = std::get_if<RawLearned>(&*next)) {
            std::string line = std::to_string(learned->id + 1);
            for (const RawRow& row : learned->rows) {
                line += " " + proof_token(row, num_vars);
            }
            lines.push_back(line + " 0" + proof_ids(learned->reasons) + " 0\n");
        } else if (const auto* empty = std::get_if<RawEmpty>(&*next)) {
            lines.push_back(std::to_string(empty->id + 1) + " 0" + proof_ids(empty->reasons) +
                            " 0\n");
        } else {
            const auto& deleted = std::get<RawDeleted>(*next);
            lines.push_back(proof_ids(deleted.ids).substr(1) + " 0\n");
        }
    }
    return lines;
}

/// @brief The solver's raw proof of @p clauses, under the default options but with BVE off, which
/// would rewrite the clauses before the graph reads them.
std::vector<std::string> solver_raw_lines(const std::vector<Clause>& clauses,
                                          std::size_t num_vars,
                                          std::size_t& num_units_fed) {
    std::ostringstream help;
    const std::optional<Options> options = parse_options({"--bve=false", "instance.xnf"}, help);
    REQUIRE(options.has_value());
    const std::filesystem::path path = scratch_path("implication_graph.raw");
    {
        RawProofWriter writer(path, num_vars, clauses.size());
        std::vector<std::size_t> every_var(num_vars);
        std::iota(every_var.begin(), every_var.end(), std::size_t{0});
        const std::unique_ptr<Solver> solver =
            make_solver(*options, num_vars, std::move(every_var), &writer);
        for (const Clause& clause : clauses) {
            solver->add_starting_clause(clause);
        }
        solver->solve();
        num_units_fed = solver->implication_graph_counters().num_units_fed;
        writer.finalize(true);
    }
    std::vector<std::string> lines = raw_lines_before_compaction(path, num_vars);
    std::filesystem::remove(path);
    return lines;
}

std::string joined(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines) {
        text += line;
    }
    return text;
}

} // namespace

// The solver's first run reads the input clauses over an empty root trail, as
// the standalone run beside it does, so that run's derivations are the ones
// the solver writes lines for: one per step, and two more per two-sided step,
// before any other line. Every line up to the first compaction that eliminates
// units checks as written, the root units the fed ones propagate included.
TEST_CASE("implication graph: the solver's raw lines check up to the first compaction") {
    TestRandom random(4242);
    std::set<ImplicationGraphProofStep::Kind> kinds;
    std::size_t num_lines_checked = 0;

    for (std::size_t trial = 0; trial < 60; ++trial) {
        const std::size_t num_vars = 3 + random.next(6);
        const std::vector<Clause> clauses = random_width_2_clauses(random, num_vars);
        // A clause spanning `0 = 1` always holds, and the solver's first
        // compaction drops it before the graph reads the database.
        if (std::ranges::any_of(clauses, [&](const Clause& clause) {
                return clause.pivot_col_idxs().test(num_vars);
            })) {
            continue;
        }
        CAPTURE(trial);

        ImplicationGraph graph(repr, num_vars);
        ImplicationGraphProofRecord record;
        const ImplicationGraphRun first =
            graph.run(clauses, all_active(clauses), RowSpace(repr, num_vars), &record);
        if (first.units.empty()) {
            continue;
        }

        std::size_t num_units_fed = 0;
        const std::vector<std::string> lines = solver_raw_lines(clauses, num_vars, num_units_fed);
        CHECK(num_units_fed >= first.units.size());
        std::size_t num_graph_lines = record.steps.size();
        for (const ImplicationGraphProofStep& step : record.steps) {
            num_graph_lines += step.kind == ImplicationGraphProofStep::Kind::both_sides ? 2 : 0;
        }
        REQUIRE(lines.size() >= num_graph_lines);
        const std::string text = joined(lines);
        LrupChecker checker(repr, num_vars, clause_rows(clauses));
        const std::string error = checker.check(text);
        CAPTURE(text);
        CHECK((error.empty() || error == "no line derives the empty clause"));
        num_lines_checked += lines.size();

        for (const ImplicationGraphProofStep* step : fed_steps(record)) {
            kinds.insert(step->kind);
        }
    }
    CHECK(num_lines_checked > 0);
    CHECK(kinds.size() == 4);
}

// Both sides of the pair for x2 + x4 are refuted, so the run feeds x2 + x4 and
// its negation, each with the line proving it; the root unit x2 = 0, which the
// first propagates before the second conflicts, cites the first's line.
TEST_CASE("implication graph: the solver's raw lines check when a run feeds a refuted pair") {
    const std::size_t num_vars = 8;
    const ParsedDimacs parsed = parse_dimacs_string(
        "p xnf 8 22\n"
        "4+6 8 0\n2+4 3 0\n-3+4 -5+6 0\n-2 5 0\n1+8 4 -5+8 0\n-2 -3 0\n7 -8 0\n"
        "-1+5 7+8 0\n-3+4 6 0\n1 3+5 5 0\n-2 -5 0\n-3+7 5 0\n-1+2 2+6 -8 0\n-4 8 0\n"
        "2+5 -6 0\n5 -7 0\n2 -6 0\n-1+6 8 0\n-3+4 4+5 0\n-1+2 -5 0\n-2+4 -4 0\n5 6 0\n",
        repr);
    REQUIRE(parsed.num_vars == num_vars);

    std::size_t num_units_fed = 0;
    const std::vector<std::string> lines =
        solver_raw_lines(parsed.clauses, num_vars, num_units_fed);
    CHECK(num_units_fed > 0);
    const std::string text = joined(lines);
    CAPTURE(text);
    LrupChecker checker(repr, num_vars, clause_rows(parsed.clauses));
    CHECK(checker.check(text).empty());
}

} // namespace xorcle
