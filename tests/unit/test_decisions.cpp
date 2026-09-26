#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "heuristics/decisions/csids.h"
#include "heuristics/decisions/decision_procedure.h"
#include "heuristics/decisions/fixed_literals.h"
#include "heuristics/decisions/phase.h"
#include "heuristics_test_support.h"
#include "util/index_set.h"
#include "util/random.h"

namespace xorcle {

TEST_CASE("make_decision_procedure: dispatches on the name") {
    const auto make = [](DecisionProcedureName name) {
        return make_decision_procedure(name, EquationRepr::dense, 4, default_seed,
                                       default_decision_phase, CsidsConfig{});
    };
    CHECK(dynamic_cast<Csids*>(make(DecisionProcedureName::csids).get()) != nullptr);
    CHECK(dynamic_cast<FixedLiterals*>(make(DecisionProcedureName::fixed_literals).get()) !=
          nullptr);
}

TEST_CASE_TEMPLATE("phase: applying a decision phase", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Equation false_equation = Equation::eq_false(repr, 4);
    Rng rng = make_rng(3, RngComponent::decision_phase);

    SUBCASE("falsify leaves the equation alone") {
        Equation eqn = make_equation_with(repr, 4, {1});
        apply_phase(eqn, DecisionPhase::falsify, false_equation, rng);
        CHECK(bits_of(eqn) == std::vector<std::size_t>{1});
    }

    SUBCASE("satisfy flips the constant column") {
        Equation eqn = make_equation_with(repr, 4, {1});
        apply_phase(eqn, DecisionPhase::satisfy, false_equation, rng);
        CHECK(bits_of(eqn) == std::vector<std::size_t>{1, 4});

        apply_phase(eqn, DecisionPhase::satisfy, false_equation, rng);
        CHECK(bits_of(eqn) == std::vector<std::size_t>{1});
    }

    SUBCASE("random is reproducible from the seed and only ever flips the constant") {
        Rng other = make_rng(3, RngComponent::decision_phase);
        for (int draw = 0; draw < 16; ++draw) {
            Equation first = make_equation_with(repr, 4, {1});
            Equation second = make_equation_with(repr, 4, {1});
            apply_phase(first, DecisionPhase::random, false_equation, rng);
            apply_phase(second, DecisionPhase::random, false_equation, other);
            CHECK(first == second);
            const std::vector<std::size_t> bits = bits_of(first);
            CHECK((bits == std::vector<std::size_t>{1} ||
                   bits == std::vector<std::size_t>{1, 4}));
        }
    }
}

TEST_CASE_TEMPLATE("fixed literals: decides variables in order", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    FixedLiterals procedure(repr, 4, default_seed);
    const std::vector<Clause> clauses;
    const std::vector<std::uint8_t> active;

    const RowSpace empty(repr, 4);
    CHECK(bits_of(*procedure.make_decision(empty, clauses, active).equation) ==
          std::vector<std::size_t>{0});

    const RowSpace span = make_row_space(repr, 4, {{0, 4}});
    CHECK(bits_of(*procedure.make_decision(span, clauses, active).equation) ==
          std::vector<std::size_t>{1});

    CHECK(procedure.counters().decisions == 2);
    CHECK(procedure.counters().fallback_decisions == 0);

    SUBCASE("reinitialize resets the counters") {
        procedure.reinitialize();
        CHECK(procedure.counters().decisions == 0);
    }

    SUBCASE("compaction rewrites the basis") {
        IndexSet dropped(5);
        dropped.set(0);
        const CompactionTransform transform = CompactionTransform::from_dropped_columns(4, dropped);
        procedure.update_with_compaction(transform, RowSpace(repr, 3), clauses, {});
        CHECK(procedure.num_vars() == 3);

        const RowSpace compacted(repr, 3);
        const Equation decision = *procedure.make_decision(compacted, clauses, active).equation;
        CHECK(decision.num_bits() == 4);
        CHECK(bits_of(decision) == std::vector<std::size_t>{0});
    }
}

TEST_CASE_TEMPLATE("csids: activity drives the clause choice", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Csids procedure(repr, 4, default_seed);
    const RowSpace empty(repr, 4);

    // Two rows apiece, so the trail leaves each clause open and the procedure
    // draws a decision from it rather than handing it back to propagation.
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, 4, {{0}, {1}}));
    clauses.push_back(make_clause(repr, 4, {{1}, {2}}));
    clauses.push_back(make_clause(repr, 4, {{2}, {3}}));
    const std::vector<std::uint8_t> active(clauses.size(), 1);
    procedure.update_with_new_clause(clauses[0], 0);
    procedure.update_with_new_clause(clauses[1], 1);

    SUBCASE("a resolved clause outranks an untouched one") {
        procedure.update_with_resolved_clause(1);
        CHECK(procedure.make_decision(empty, clauses, active).equation.has_value());
        CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{1});
        CHECK(procedure.counters().fallback_decisions == 0);
    }

    SUBCASE("a new asserting clause enters above every starting clause") {
        procedure.update_with_asserting_clause(clauses[2]);
        procedure.update_with_new_clause(clauses[2], 2);
        CHECK(procedure.make_decision(empty, clauses, active).equation.has_value());
        CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{2});
    }

    SUBCASE("the satisfy phase flips the sampled equation's constant column") {
        Csids satisfying(repr, 4, default_seed);
        satisfying.update_with_new_clause(clauses[0], 0);
        satisfying.update_with_new_clause(clauses[1], 1);
        satisfying.set_decision_phase(DecisionPhase::satisfy);

        procedure.update_with_resolved_clause(0);
        satisfying.update_with_resolved_clause(0);

        // Both draw the same combination from the same stream, so they differ in
        // the constant column alone.
        Equation falsifying_decision = *procedure.make_decision(empty, clauses, active).equation;
        const Equation satisfying_decision =
            *satisfying.make_decision(empty, clauses, active).equation;
        falsifying_decision.plus_assign(Equation::eq_false(repr, 4));
        CHECK(falsifying_decision == satisfying_decision);
    }

    SUBCASE("satisfied clauses are dropped from the heap") {
        procedure.update_with_resolved_clause(0);
        // x0 = 1 satisfies clause 0, so the next highest clause is used instead.
        const RowSpace span = make_row_space(repr, 4, {{0, 4}});
        CHECK(procedure.make_decision(span, clauses, active).equation.has_value());
        CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{1});
    }

    SUBCASE("a backjump offers the clauses the discarded levels satisfied again") {
        procedure.update_with_resolved_clause(0);
        // The first decision takes the trail to level 1, where x0 = 1 satisfies
        // clause 0, which leaves the heap on the way past it.
        CHECK(procedure.make_decision(empty, clauses, active).equation.has_value());
        const RowSpace span = make_row_space(repr, 4, {{0, 4}});
        CHECK(procedure.make_decision(span, clauses, active).equation.has_value());
        CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{1});

        SUBCASE("keeping the level that satisfied it keeps it off the heap") {
            procedure.update_with_backjump(1);
            CHECK(procedure.make_decision(empty, clauses, active).equation.has_value());
            CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{1});
        }

        SUBCASE("discarding that level puts it back") {
            // Clause 0 outranks clause 1 again -- but only because it is back
            // in the heap.
            procedure.update_with_backjump(0);
            CHECK(procedure.make_decision(empty, clauses, active).equation.has_value());
            CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{0});
        }
    }

    SUBCASE("a clause propagation found satisfied is set aside at the level that did") {
        procedure.update_with_resolved_clause(0);
        // The record says level 0 satisfies clause 0, which the procedure takes
        // on trust rather than checking against the trail.
        const std::vector<std::size_t> stamps{1};
        clauses[0].satisfied_level = 0;
        clauses[0].satisfied_stamp = 1;
        CHECK(procedure.make_decision(empty, clauses, active, SatisfactionRecord(stamps, 0))
                  .equation.has_value());
        CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{1});

        SUBCASE("it stays off the heap while that level stands") {
            procedure.update_with_backjump(0);
            CHECK(procedure.make_decision(empty, clauses, active, SatisfactionRecord(stamps, 0))
                      .equation.has_value());
            CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{1});
        }
    }

    SUBCASE("a record whose level has since been replaced is not trusted") {
        procedure.update_with_resolved_clause(0);
        const std::vector<std::size_t> stamps{7};
        clauses[0].satisfied_level = 0;
        clauses[0].satisfied_stamp = 1;
        CHECK(procedure.make_decision(empty, clauses, active, SatisfactionRecord(stamps, 0))
                  .equation.has_value());
        CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{0});
    }

    SUBCASE("a clause the trail has left unit goes back to propagation") {
        procedure.update_with_resolved_clause(0);
        // x1 = 0 kills one row of clause 0, leaving the other as its only
        // surviving dimension.
        const RowSpace span = make_row_space(repr, 4, {{1}});
        const DecisionResult result = procedure.make_decision(span, clauses, active);
        CHECK_FALSE(result.equation.has_value());
        CHECK(result.propagating_clause == std::optional<ClauseId>{0});
        // It names no decision, so the record from before stands.
        CHECK(procedure.decision_clause_hint() == std::nullopt);
        CHECK(procedure.counters().decisions == 0);
    }

    SUBCASE("a clause the trail refutes goes back to propagation") {
        procedure.update_with_resolved_clause(0);
        const RowSpace span = make_row_space(repr, 4, {{0}, {1}});
        const DecisionResult result = procedure.make_decision(span, clauses, active);
        CHECK_FALSE(result.equation.has_value());
        CHECK(result.propagating_clause == std::optional<ClauseId>{0});
    }

    SUBCASE("an inactive clause is passed over and never offered again") {
        procedure.update_with_resolved_clause(1);
        std::vector<std::uint8_t> deleted = active;
        deleted[1] = 0;
        CHECK(procedure.make_decision(empty, clauses, deleted).equation.has_value());
        CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{0});

        // Deactivation is permanent, so the heap drops the clause outright, and
        // its activity stays readable for deletion and subsumption.
        CHECK(procedure.clause_activity(1) > procedure.clause_activity(0));
        procedure.update_with_backjump(0);
        CHECK(procedure.make_decision(empty, clauses, active).equation.has_value());
        CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{0});
    }

    SUBCASE("an exhausted heap offers no decision") {
        Csids fallback(repr, 4, default_seed);
        const std::vector<Clause> no_clauses;
        const std::vector<std::uint8_t> no_active;
        // Every active clause is satisfied, so the solver completes the model
        // rather than deciding the rest one variable at a time.
        CHECK_FALSE(fallback.make_decision(empty, no_clauses, no_active).equation.has_value());
        CHECK(fallback.decision_clause_hint() == std::nullopt);
        CHECK(fallback.counters().decisions == 0);

        fallback.note_filled_decisions(4);
        CHECK(fallback.counters().decisions == 4);
        CHECK(fallback.counters().fallback_decisions == 4);
    }

    SUBCASE("reinitialize restores the sampling stream") {
        procedure.update_with_resolved_clause(1);
        const Equation first = *procedure.make_decision(empty, clauses, active).equation;
        procedure.reinitialize();
        procedure.update_with_new_clause(clauses[0], 0);
        procedure.update_with_new_clause(clauses[1], 1);
        procedure.update_with_resolved_clause(1);
        CHECK(*procedure.make_decision(empty, clauses, active).equation == first);
        CHECK(procedure.counters().decisions == 1);
    }

    SUBCASE("compaction adopts the new variable count and forgets a dropped decision clause") {
        procedure.update_with_resolved_clause(1);
        (void)procedure.make_decision(empty, clauses, active);
        REQUIRE(procedure.decision_clause_hint() == std::optional<ClauseId>{1});

        IndexSet dropped(5);
        dropped.set(0);
        const CompactionTransform transform = CompactionTransform::from_dropped_columns(4, dropped);
        const std::vector<std::uint8_t> clause_active{1, 0, 1};
        procedure.update_with_compaction(transform, RowSpace(repr, 3), clauses, clause_active);

        CHECK(procedure.num_vars() == 3);
        CHECK(procedure.decision_clause_hint() == std::nullopt);
    }
}

TEST_CASE_TEMPLATE("csids: decay makes later bumps worth more", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Csids procedure(repr, 4, default_seed);
    const RowSpace empty(repr, 4);

    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, 4, {{0}, {1}}));
    clauses.push_back(make_clause(repr, 4, {{1}, {2}}));
    clauses.push_back(make_clause(repr, 4, {{2}, {3}}));
    const std::vector<std::uint8_t> active(clauses.size(), 1);
    procedure.update_with_new_clause(clauses[0], 0);
    procedure.update_with_new_clause(clauses[1], 1);

    // Rather than decaying every score per conflict, the increment grows by
    // 1/alpha, so of two bumps either side of a conflict the later is worth
    // strictly more.
    procedure.update_with_resolved_clause(0);
    procedure.update_with_asserting_clause(clauses[2]);
    procedure.update_with_new_clause(clauses[2], 2);
    procedure.update_with_resolved_clause(1);

    CHECK(procedure.make_decision(empty, clauses, active).equation.has_value());
    CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{1});
}

TEST_CASE_TEMPLATE("csids: a deeper intersection narrows the top clause's decision", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    CsidsConfig config;
    config.intersection_depth = 2;
    Csids procedure(repr, 4, default_seed, config);
    const RowSpace empty(repr, 4);

    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, 4, {{0}, {1}}));
    clauses.push_back(make_clause(repr, 4, {{1}, {2}}));
    clauses.push_back(make_clause(repr, 4, {{2}, {3}}));
    const std::vector<std::uint8_t> active(clauses.size(), 1);
    for (ClauseId id = 0; id < clauses.size(); ++id) {
        procedure.update_with_new_clause(clauses[id], id);
    }

    SUBCASE("the intersection with the next clause is kept") {
        procedure.update_with_resolved_clause(0);
        const DecisionResult result = procedure.make_decision(empty, clauses, active);
        REQUIRE(result.equation.has_value());
        CHECK(bits_of(*result.equation) == std::vector<std::size_t>{1});
        CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{0});

        const IntersectionCounters counters = procedure.intersection_counters();
        CHECK(counters.decisions == 1);
        CHECK(counters.intersected_clauses == 1);
        CHECK(counters.dimension_before == 2);
        CHECK(counters.dimension_after == 1);
    }

    SUBCASE("with nothing kept the draw is the top clause's alone") {
        CsidsConfig top_alone;
        top_alone.intersection_depth = 1;
        Csids plain(repr, 4, default_seed, top_alone);
        for (ClauseId id = 0; id < clauses.size(); ++id) {
            plain.update_with_new_clause(clauses[id], id);
        }
        // Clause 0 then clause 2, whose spans meet only in `0 = 0`.
        const std::vector<std::uint8_t> without_middle{1, 0, 1};
        procedure.update_with_resolved_clause(0);
        plain.update_with_resolved_clause(0);
        const DecisionResult deep = procedure.make_decision(empty, clauses, without_middle);
        const DecisionResult top = plain.make_decision(empty, clauses, without_middle);
        REQUIRE(deep.equation.has_value());
        REQUIRE(top.equation.has_value());
        CHECK(*deep.equation == *top.equation);
        CHECK(procedure.intersection_counters().decisions == 0);
    }

    SUBCASE("a propagating clause below the top is handed back") {
        procedure.update_with_resolved_clause(0);
        const RowSpace span = make_row_space(repr, 4, {{2}});
        const DecisionResult result = procedure.make_decision(span, clauses, active);
        CHECK(!result.equation.has_value());
        CHECK(result.propagating_clause == std::optional<ClauseId>{1});
        CHECK(procedure.intersection_counters().propagating_clauses == 1);

        // Nothing it set aside is lost: off that trail, both clauses are back.
        const DecisionResult next = procedure.make_decision(empty, clauses, active);
        REQUIRE(next.equation.has_value());
        CHECK(bits_of(*next.equation) == std::vector<std::size_t>{1});
        CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{0});
    }
}

TEST_CASE_TEMPLATE("decision procedure: the default keeps no levels on a restart", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const std::vector<Clause> clauses{make_clause(repr, 4, {{0}})};
    const std::vector<std::uint8_t> active(clauses.size(), 1);
    const RowSpace span = make_row_space(repr, 4, {});

    FixedLiterals decisions(repr, 4, Seed{1});
    CHECK(decisions.reusable_levels(span, clauses, active, {ClauseId{0}}) == 0);
}

TEST_CASE_TEMPLATE("csids: trail reuse keeps the levels outranking the next decision", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    constexpr std::size_t num_vars = 4;

    // Clause `i` is `[x_i = 1] v [x_(i+1) = 1]`, so a trail row `x_i + 1 = 0`
    // satisfies it while leaving the clauses after it open.
    std::vector<Clause> clauses;
    for (std::size_t var = 0; var < 3; ++var) {
        clauses.push_back(make_clause(repr, num_vars, {{var}, {var + 1}}));
    }

    const std::vector<std::uint8_t> active(clauses.size(), 1);

    Csids decisions(repr, num_vars, Seed{1});
    for (ClauseId id = 0; id < clauses.size(); ++id) {
        decisions.update_with_new_clause(clauses[id], id);
    }
    // Activity 3, 2 and 0 respectively, all at the same increment.
    decisions.update_with_resolved_clause(0);
    decisions.update_with_resolved_clause(0);
    decisions.update_with_resolved_clause(0);
    decisions.update_with_resolved_clause(1);
    decisions.update_with_resolved_clause(1);

    // The trail satisfies clause 0 at level 1, so clause 1 is what the next
    // decision comes from and only a level decided from clause 0 outranks it.
    const RowSpace empty = make_row_space(repr, num_vars, {});
    (void)decisions.make_decision(empty, clauses, active);
    const RowSpace span = make_row_space(repr, num_vars, {{0, num_vars}});

    CHECK(decisions.reusable_levels(span, clauses, active, {ClauseId{0}}) == 1);
    CHECK(decisions.reusable_levels(span, clauses, active, {ClauseId{0}, ClauseId{1}}) == 1);
    CHECK(decisions.reusable_levels(span, clauses, active, {ClauseId{1}}) == 0);
    CHECK(decisions.reusable_levels(span, clauses, active, {ClauseId{2}}) == 0);

    SUBCASE("a level decided from a deleted clause ends the run of kept levels") {
        std::vector<std::uint8_t> deleted = active;
        deleted[0] = 0;
        CHECK(decisions.reusable_levels(span, clauses, deleted, {ClauseId{0}}) == 0);
    }

    SUBCASE("a level the fallback basis decided ends the run of kept levels") {
        CHECK(decisions.reusable_levels(span, clauses, active, {std::nullopt}) == 0);
        CHECK(decisions.reusable_levels(span, clauses, active, {ClauseId{0}, std::nullopt}) == 1);
    }

    SUBCASE("the restart's backjump leaves the choice of clause where it was") {
        // Clause 0 was walked past on the satisfying trail; once the restart
        // discards level 1, it must again be the clause with the most activity.
        decisions.update_with_backjump(0);
        (void)decisions.make_decision(empty, clauses, active);
        CHECK(decisions.decision_clause_hint() == std::optional<ClauseId>{0});
    }
}

} // namespace xorcle
