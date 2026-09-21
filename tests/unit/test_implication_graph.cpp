#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "heuristics/implication_graph/implication_graph.h"
#include "heuristics_test_support.h"

namespace xorcle {

namespace {

constexpr EquationRepr repr = EquationRepr::dense;

/// @brief The active flags of a database in which every clause is live.
std::vector<std::uint8_t> all_active(const std::vector<Clause>& clauses) {
    return std::vector<std::uint8_t>(clauses.size(), 1);
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

/// @brief The steps of @p record that carry a unit, in the order they do.
std::vector<const ImplicationGraphProofStep*> fed_steps(
    const ImplicationGraphProofRecord& record) {
    std::vector<const ImplicationGraphProofStep*> fed;
    for (const ImplicationGraphProofStep& step : record.steps) {
        if (step.fed) {
            fed.push_back(&step);
        }
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
    std::vector<Equation> units;
    const ImplicationGraphRun summary =
        graph.run(clauses, all_active(clauses), RowSpace(repr, num_vars), units);

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
    std::vector<Equation> units;
    const ImplicationGraphRun summary =
        graph.run(clauses, all_active(clauses), RowSpace(repr, num_vars), units);

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
    std::vector<Equation> units;
    const ImplicationGraphRun summary =
        graph.run(clauses, all_active(clauses), RowSpace(repr, num_vars), units);

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
    std::vector<Equation> units;
    const ImplicationGraphRun summary =
        graph.run(clauses, all_active(clauses), RowSpace(repr, num_vars), units);

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
    std::vector<Equation> units;
    const ImplicationGraphRun summary = graph.run(clauses, all_active(clauses), root_span, units);

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
    std::vector<Equation> units;
    graph.run(clauses, all_active(clauses), RowSpace(repr, num_vars), units);

    CHECK(graph.num_width_2_clauses() == 0);
    CHECK(graph.num_vertex_pairs() == 0);
    CHECK(units.empty());
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
    std::vector<Equation> units;
    graph.run(clauses, all_active(clauses), root_span, units);

    CHECK(units.empty());
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
    std::vector<Equation> first;
    CHECK(graph.run(clauses, all_active(clauses), root_span, first).did_work);
    REQUIRE(first.empty());

    clauses.push_back(make_clause(repr, num_vars, {{4, 5}, {2}}));
    std::vector<Equation> units;
    const ImplicationGraphRun summary = graph.run(clauses, all_active(clauses), root_span, units);

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
    std::vector<Equation> all_units;
    const ImplicationGraphRun full =
        uncapped.run(clauses, all_active(clauses), root_span, all_units);
    REQUIRE(full.pairs_eligible > 2);
    CHECK(full.pairs_examined == full.pairs_eligible);
    CHECK(!full.at_the_pair_cap);
    CHECK(full.truncated_sides == 0);
    const RowSpace implied = span_of(num_vars, all_units);

    SUBCASE("the pair cap stops the walk at the pairs it sampled") {
        ImplicationGraph graph(repr, num_vars, ImplicationGraphConfig{.max_pairs = 2});
        std::vector<Equation> units;
        const ImplicationGraphRun capped = graph.run(clauses, all_active(clauses), root_span, units);

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
        std::vector<Equation> units;
        const ImplicationGraphRun capped = graph.run(clauses, all_active(clauses), root_span, units);

        CHECK(capped.pairs_examined == full.pairs_eligible);
        CHECK(capped.truncated_sides > 0);
        CHECK(units.size() <= all_units.size());
        for (const Equation& unit : units) {
            CHECK(holds(implied, num_vars, unit.bits()));
        }
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
    std::vector<Equation> units;
    ImplicationGraphProofRecord record;
    graph.run(clauses, all_active(clauses), RowSpace(repr, num_vars), units, &record);

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
    std::vector<Equation> units;
    ImplicationGraphProofRecord record;
    graph.run(clauses, all_active(clauses), RowSpace(repr, num_vars), units, &record);

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
    std::vector<Equation> units;
    const ImplicationGraphRun summary = graph.run(clauses, all_active(clauses), root_span, units);

    CHECK(summary.did_work);
    CHECK(graph.num_width_2_clauses() == 3);
    CHECK(units.empty());
}

} // namespace xorcle
