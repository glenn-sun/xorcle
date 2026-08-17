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
#include "util/bitmap.h"
#include "util/random.h"

namespace xorcle {

TEST_CASE("csids: the presets carry their documented values") {
    // The declaration order is (A0, alpha, delta), while the triple is usually
    // written (A0, delta, alpha). These check the members by name, which is the
    // only way the two orders cannot be confused.
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
    constexpr Algebra backend = Tag::value;
    const Equation false_equation = Equation::eq_false(backend, 4);
    Rng rng = make_rng(3, RngComponent::decision_phase);

    SUBCASE("falsify leaves the equation alone") {
        Equation eqn = make_equation_with(backend, 4, {1});
        apply_phase(eqn, DecisionPhase::falsify, false_equation, rng);
        CHECK(columns_of(eqn) == std::vector<std::size_t>{1});
    }

    SUBCASE("satisfy flips the constant column") {
        Equation eqn = make_equation_with(backend, 4, {1});
        apply_phase(eqn, DecisionPhase::satisfy, false_equation, rng);
        CHECK(columns_of(eqn) == std::vector<std::size_t>{1, 4});

        apply_phase(eqn, DecisionPhase::satisfy, false_equation, rng);
        CHECK(columns_of(eqn) == std::vector<std::size_t>{1});
    }

    SUBCASE("random is reproducible from the seed and only ever flips the constant") {
        Rng other = make_rng(3, RngComponent::decision_phase);
        for (int draw = 0; draw < 16; ++draw) {
            Equation first = make_equation_with(backend, 4, {1});
            Equation second = make_equation_with(backend, 4, {1});
            apply_phase(first, DecisionPhase::random, false_equation, rng);
            apply_phase(second, DecisionPhase::random, false_equation, other);
            CHECK(first == second);
            const std::vector<std::size_t> columns = columns_of(first);
            CHECK((columns == std::vector<std::size_t>{1} ||
                   columns == std::vector<std::size_t>{1, 4}));
        }
    }

    SUBCASE("falsify is the default") {
        CHECK(default_decision_phase == DecisionPhase::falsify);
        const FixedLiterals procedure(backend, 4, default_seed);
        CHECK(procedure.decision_phase() == DecisionPhase::falsify);
    }
}

TEST_CASE_TEMPLATE("fixed literals: decides variables in order", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    FixedLiterals procedure(backend, 4, default_seed);
    const std::vector<Clause> clauses;

    const RowSpace empty(backend, 4);
    CHECK(columns_of(procedure.make_decision(empty, clauses)) == std::vector<std::size_t>{0});

    const RowSpace span = make_row_space(backend, 4, {{0, 4}});
    CHECK(columns_of(procedure.make_decision(span, clauses)) == std::vector<std::size_t>{1});

    CHECK(procedure.decision_stats().decisions == 2);
    CHECK(procedure.decision_stats().fallback_decisions == 0);

    SUBCASE("reinitialize resets the counters") {
        procedure.reinitialize();
        CHECK(procedure.decision_stats().decisions == 0);
    }

    SUBCASE("compaction reprojects the basis") {
        Bitmap pivots(5);
        pivots.set(0);
        const CompactionTransform transform = CompactionTransform::from_pivots(4, pivots);
        procedure.update_with_compaction(transform, RowSpace(backend, 3), clauses, {});
        CHECK(procedure.num_vars() == 3);

        const RowSpace compacted(backend, 3);
        const Equation decision = procedure.make_decision(compacted, clauses);
        CHECK(decision.size() == 4);
        CHECK(columns_of(decision) == std::vector<std::size_t>{0});
    }
}

TEST_CASE_TEMPLATE("variable vsids: activity drives the choice", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    VariableVsids procedure(backend, 4, default_seed);
    const std::vector<Clause> clauses;
    const RowSpace empty(backend, 4);

    SUBCASE("bumping an asserting clause's rows moves them to the front") {
        procedure.update_with_asserting_clause(make_clause(backend, 4, {{2}}));
        CHECK(columns_of(procedure.make_decision(empty, clauses)) == std::vector<std::size_t>{2});
    }

    SUBCASE("critical equations are bumped by their leading variable") {
        procedure.update_with_critical_equation(make_equation_with(backend, 4, {3, 4}));
        // The heap only reorders on rebuild, which an asserting clause triggers.
        procedure.update_with_asserting_clause(make_clause(backend, 4, {{3}}));
        CHECK(columns_of(procedure.make_decision(empty, clauses)) == std::vector<std::size_t>{3});
    }

    SUBCASE("determined variables are skipped and the decision comes back reduced") {
        procedure.update_with_asserting_clause(make_clause(backend, 4, {{0}}));
        const RowSpace span = make_row_space(backend, 4, {{0, 1}});
        // x0 is the highest-activity literal but reduces to x1, which is legal.
        CHECK(columns_of(procedure.make_decision(span, clauses)) == std::vector<std::size_t>{1});
        CHECK(procedure.decision_stats().decisions == 1);
    }

    SUBCASE("compaction remaps the heap and keeps a legal decision available") {
        procedure.update_with_asserting_clause(make_clause(backend, 4, {{2}}));

        Bitmap pivots(5);
        pivots.set(0);
        const CompactionTransform transform = CompactionTransform::from_pivots(4, pivots);
        procedure.update_with_compaction(transform, RowSpace(backend, 3), clauses, {});
        CHECK(procedure.num_vars() == 3);

        const RowSpace compacted(backend, 3);
        const Equation decision = procedure.make_decision(compacted, clauses);
        CHECK(decision.size() == 4);
        // Old x2 became new x1, and it kept its activity lead.
        CHECK(columns_of(decision) == std::vector<std::size_t>{1});
    }
}

TEST_CASE_TEMPLATE("csids: activity drives the clause choice", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    Csids procedure(backend, 4, default_seed);
    const RowSpace empty(backend, 4);

    std::vector<Clause> clauses;
    clauses.push_back(make_clause(backend, 4, {{0}}));
    clauses.push_back(make_clause(backend, 4, {{1}}));
    clauses.push_back(make_clause(backend, 4, {{2}}));
    procedure.update_with_new_clause(clauses[0], 0);
    procedure.update_with_new_clause(clauses[1], 1);

    SUBCASE("a resolved clause outranks an untouched one") {
        procedure.update_with_resolved_clause(1);
        CHECK(columns_of(procedure.make_decision(empty, clauses)) == std::vector<std::size_t>{1});
        CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{1});
        CHECK(procedure.decision_stats().fallback_decisions == 0);
    }

    SUBCASE("a new asserting clause enters above every starting clause") {
        procedure.update_with_asserting_clause(clauses[2]);
        procedure.update_with_new_clause(clauses[2], 2);
        CHECK(columns_of(procedure.make_decision(empty, clauses)) == std::vector<std::size_t>{2});
        CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{2});
    }

    SUBCASE("the satisfy phase flips the sampled equation's constant column") {
        procedure.set_decision_phase(DecisionPhase::satisfy);
        procedure.update_with_resolved_clause(0);
        CHECK(columns_of(procedure.make_decision(empty, clauses)) ==
              std::vector<std::size_t>{0, 4});
    }

    SUBCASE("satisfied clauses are dropped from the heap") {
        procedure.update_with_resolved_clause(0);
        // x0 = 1 satisfies clause 0, so the next highest clause is used instead.
        const RowSpace span = make_row_space(backend, 4, {{0, 4}});
        CHECK(columns_of(procedure.make_decision(span, clauses)) == std::vector<std::size_t>{1});
        CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{1});
    }

    SUBCASE("an exhausted heap falls back to the literal basis") {
        Csids fallback(backend, 4, default_seed);
        const std::vector<Clause> no_clauses;
        CHECK(columns_of(fallback.make_decision(empty, no_clauses)) ==
              std::vector<std::size_t>{0});
        CHECK(fallback.decision_clause_hint() == std::nullopt);
        CHECK(fallback.decision_stats().decisions == 1);
        CHECK(fallback.decision_stats().fallback_decisions == 1);
    }

    SUBCASE("reinitialize restores the sampling stream") {
        procedure.update_with_resolved_clause(1);
        const Equation first = procedure.make_decision(empty, clauses);
        procedure.reinitialize();
        procedure.update_with_new_clause(clauses[0], 0);
        procedure.update_with_new_clause(clauses[1], 1);
        procedure.update_with_resolved_clause(1);
        CHECK(procedure.make_decision(empty, clauses) == first);
        CHECK(procedure.decision_stats().decisions == 1);
    }

    SUBCASE("compaction adopts the new width and forgets a dropped decision clause") {
        procedure.update_with_resolved_clause(1);
        (void)procedure.make_decision(empty, clauses);
        REQUIRE(procedure.decision_clause_hint() == std::optional<ClauseId>{1});

        Bitmap pivots(5);
        pivots.set(0);
        const CompactionTransform transform = CompactionTransform::from_pivots(4, pivots);
        const std::vector<std::uint8_t> clause_active{1, 0, 1};
        procedure.update_with_compaction(transform, RowSpace(backend, 3), clauses, clause_active);

        CHECK(procedure.num_vars() == 3);
        CHECK(procedure.decision_clause_hint() == std::nullopt);
    }
}

TEST_CASE_TEMPLATE("csids: decay makes later bumps worth more", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    Csids procedure(backend, 4, default_seed);
    const RowSpace empty(backend, 4);

    std::vector<Clause> clauses;
    clauses.push_back(make_clause(backend, 4, {{0}}));
    clauses.push_back(make_clause(backend, 4, {{1}}));
    clauses.push_back(make_clause(backend, 4, {{2}}));
    procedure.update_with_new_clause(clauses[0], 0);
    procedure.update_with_new_clause(clauses[1], 1);

    // Rather than decaying every score per conflict, the increment grows by
    // 1/alpha. A bump on either side of a conflict is the observable
    // consequence: the later one is worth strictly more.
    procedure.update_with_resolved_clause(0);
    procedure.update_with_asserting_clause(clauses[2]);
    procedure.update_with_new_clause(clauses[2], 2);
    procedure.update_with_resolved_clause(1);

    CHECK(columns_of(procedure.make_decision(empty, clauses)) == std::vector<std::size_t>{1});
    CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{1});
}

} // namespace xorcle
