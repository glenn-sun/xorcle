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
#include "heuristics/decisions/variable_vsids.h"
#include "heuristics_test_support.h"
#include "util/index_set.h"
#include "util/random.h"

namespace xorcle {

TEST_CASE("csids: the presets carry their documented values") {
    // The declaration order is (A0, alpha, delta), while the triple is usually
    // written (A0, delta, alpha), so these check the members by name.
    CHECK(csids_default_config.initial_activity == doctest::Approx(1.0));
    CHECK(csids_default_config.bump == doctest::Approx(1.0));
    CHECK(csids_default_config.decay == doctest::Approx(0.98));

    CHECK(csids_berkmin_config.initial_activity == doctest::Approx(1.0));
    CHECK(csids_berkmin_config.bump == doctest::Approx(0.0));
    CHECK(csids_berkmin_config.decay == doctest::Approx(0.5));

    CHECK(csids_cmtf_config.initial_activity == doctest::Approx(1.0));
    CHECK(csids_cmtf_config.bump == doctest::Approx(1.0));
    CHECK(csids_cmtf_config.decay == doctest::Approx(0.25));
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

    SUBCASE("falsify is the default") {
        CHECK(default_decision_phase == DecisionPhase::falsify);
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

    CHECK(procedure.decision_stats().decisions == 2);
    CHECK(procedure.decision_stats().fallback_decisions == 0);

    SUBCASE("reinitialize resets the counters") {
        procedure.reinitialize();
        CHECK(procedure.decision_stats().decisions == 0);
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

TEST_CASE_TEMPLATE("variable vsids: activity drives the choice", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    VariableVsids procedure(repr, 4, default_seed);
    const std::vector<Clause> clauses;
    const std::vector<std::uint8_t> active;
    const RowSpace empty(repr, 4);

    SUBCASE("bumping an asserting clause's rows moves them to the front") {
        procedure.update_with_asserting_clause(make_clause(repr, 4, {{2}}));
        CHECK(bits_of(*procedure.make_decision(empty, clauses, active).equation) ==
              std::vector<std::size_t>{2});
    }

    SUBCASE("critical equations are bumped by their leading variable") {
        procedure.update_with_critical_equation(make_equation_with(repr, 4, {3, 4}));
        // The heap only reorders on rebuild, which an asserting clause triggers.
        procedure.update_with_asserting_clause(make_clause(repr, 4, {{3}}));
        CHECK(bits_of(*procedure.make_decision(empty, clauses, active).equation) ==
              std::vector<std::size_t>{3});
    }

    SUBCASE("determined variables are skipped and the decision comes back reduced") {
        procedure.update_with_asserting_clause(make_clause(repr, 4, {{0}}));
        const RowSpace span = make_row_space(repr, 4, {{0, 1}});
        // x0 is the highest-activity literal but reduces to x1, which is legal.
        CHECK(bits_of(*procedure.make_decision(span, clauses, active).equation) ==
              std::vector<std::size_t>{1});
        CHECK(procedure.decision_stats().decisions == 1);
    }

    SUBCASE("compaction remaps the heap and keeps a legal decision available") {
        procedure.update_with_asserting_clause(make_clause(repr, 4, {{2}}));

        IndexSet dropped(5);
        dropped.set(0);
        const CompactionTransform transform = CompactionTransform::from_dropped_columns(4, dropped);
        procedure.update_with_compaction(transform, RowSpace(repr, 3), clauses, {});
        CHECK(procedure.num_vars() == 3);

        const RowSpace compacted(repr, 3);
        const Equation decision = *procedure.make_decision(compacted, clauses, active).equation;
        CHECK(decision.num_bits() == 4);
        // Old x2 became new x1, and it kept its activity lead.
        CHECK(bits_of(decision) == std::vector<std::size_t>{1});
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
        CHECK(procedure.decision_stats().fallback_decisions == 0);
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

    SUBCASE("an undone decision offers the clauses that level satisfied again") {
        procedure.update_with_resolved_clause(0);
        // x0 = 1 satisfies clause 0, which leaves the heap on the way past it.
        const RowSpace span = make_row_space(repr, 4, {{0, 4}});
        CHECK(procedure.make_decision(span, clauses, active).equation.has_value());
        CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{1});

        // With the level that satisfied it gone, clause 0 outranks clause 1
        // again -- but only because it is back in the heap.
        procedure.update_with_undone_decision();
        CHECK(procedure.make_decision(empty, clauses, active).equation.has_value());
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
        CHECK(procedure.decision_stats().decisions == 0);
    }

    SUBCASE("a clause the trail refutes goes back to propagation") {
        procedure.update_with_resolved_clause(0);
        const RowSpace span = make_row_space(repr, 4, {{0}, {1}});
        const DecisionResult result = procedure.make_decision(span, clauses, active);
        CHECK_FALSE(result.equation.has_value());
        CHECK(result.propagating_clause == std::optional<ClauseId>{0});
    }

    SUBCASE("an inactive clause is passed over") {
        procedure.update_with_resolved_clause(1);
        std::vector<std::uint8_t> deleted = active;
        deleted[1] = 0;
        CHECK(procedure.make_decision(empty, clauses, deleted).equation.has_value());
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
        CHECK(fallback.decision_stats().decisions == 0);

        fallback.note_filled_decisions(4);
        CHECK(fallback.decision_stats().decisions == 4);
        CHECK(fallback.decision_stats().fallback_decisions == 4);
    }

    SUBCASE("reinitialize restores the sampling stream") {
        procedure.update_with_resolved_clause(1);
        const Equation first = *procedure.make_decision(empty, clauses, active).equation;
        procedure.reinitialize();
        procedure.update_with_new_clause(clauses[0], 0);
        procedure.update_with_new_clause(clauses[1], 1);
        procedure.update_with_resolved_clause(1);
        CHECK(*procedure.make_decision(empty, clauses, active).equation == first);
        CHECK(procedure.decision_stats().decisions == 1);
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

TEST_CASE_TEMPLATE("decision procedure: the default keeps no levels on a restart", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const std::vector<Clause> clauses{make_clause(repr, 4, {{0}})};
    const std::vector<std::uint8_t> active(clauses.size(), 1);
    const RowSpace span = make_row_space(repr, 4, {});

    VariableVsids decisions(repr, 4, Seed{1});
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

    // The trail satisfies clause 0, so clause 1 is what the next decision comes
    // from and only a level decided from clause 0 outranks it.
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

    SUBCASE("asking leaves the choice of clause where it was") {
        // Clause 0 was walked past on the satisfying trail; on the empty trail
        // it must still be the clause with the most activity.
        const RowSpace empty = make_row_space(repr, num_vars, {});
        (void)decisions.make_decision(empty, clauses, active);
        CHECK(decisions.decision_clause_hint() == std::optional<ClauseId>{0});
    }
}

} // namespace xorcle
