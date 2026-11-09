#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/trail.h"
#include "algebra_test_support.h"
#include "heuristics/decisions/csids.h"
#include "heuristics/decisions/decision_procedure.h"
#include "heuristics/decisions/fixed_literals.h"
#include "heuristics/decisions/phase.h"
#include "heuristics/decisions/saved_trails.h"
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

    SUBCASE("a substitution takes the dropped variable out of the basis") {
        IndexSet dropped(5);
        dropped.set(0);
        procedure.update_with_substitution(dropped, clauses, {});
        CHECK(procedure.num_vars() == 4);
        CHECK(bits_of(*procedure.make_decision(empty, clauses, active).equation) ==
              std::vector<std::size_t>{1});
    }

    SUBCASE("a renumbering rewrites the basis") {
        IndexSet dropped(5);
        dropped.set(0);
        const CompactionTransform transform = CompactionTransform::from_dropped_columns(4, dropped);
        procedure.update_with_substitution(dropped, clauses, {});
        procedure.update_with_renumbering(transform);
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
    procedure.set_decision_phase(DecisionPhase::falsify);
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
        procedure.update_with_substitution(dropped, clauses, clause_active);
        CHECK(procedure.num_vars() == 4);
        CHECK(procedure.decision_clause_hint() == std::nullopt);

        procedure.update_with_renumbering(transform);
        CHECK(procedure.num_vars() == 3);
    }
}

TEST_CASE_TEMPLATE("csids: decay makes later bumps worth more", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Csids procedure(repr, 4, default_seed);
    procedure.set_decision_phase(DecisionPhase::falsify);
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
    procedure.set_decision_phase(DecisionPhase::falsify);
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
        CHECK(counters.empty_intersections == 0);
    }

    SUBCASE("a clause sharing no pivot column is passed over") {
        config.intersection_depth = 3;
        Csids deeper(repr, 4, default_seed, config);
        deeper.set_decision_phase(DecisionPhase::falsify);
        for (ClauseId id = 0; id < clauses.size(); ++id) {
            deeper.update_with_new_clause(clauses[id], id);
        }
        deeper.update_with_resolved_clause(0);
        deeper.update_with_resolved_clause(0);
        deeper.update_with_resolved_clause(2);
        const DecisionResult result = deeper.make_decision(empty, clauses, active);
        REQUIRE(result.equation.has_value());
        CHECK(bits_of(*result.equation) == std::vector<std::size_t>{1});

        const IntersectionCounters counters = deeper.intersection_counters();
        CHECK(counters.intersected_clauses == 1);
        CHECK(counters.empty_intersections == 1);
        CHECK(counters.skipped_intersections == 1);
    }

    SUBCASE("with nothing kept the draw is the top clause's alone") {
        CsidsConfig top_alone;
        top_alone.intersection_depth = 1;
        Csids plain(repr, 4, default_seed, top_alone);
        plain.set_decision_phase(DecisionPhase::falsify);
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

TEST_CASE_TEMPLATE("csids: span^± intersects directions and draws the side", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    constexpr std::size_t num_vars = 4;
    const RowSpace empty(repr, num_vars);

    // `span(x0, x1)` and `span(x0 + 1, x2)` meet only in `0 = 0`; with `0 = 1`
    // added to both they share the direction `x0`.
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{0}, {1}}));
    clauses.push_back(make_clause(repr, num_vars, {{0, num_vars}, {2}}));
    const std::vector<std::uint8_t> active(clauses.size(), 1);

    CsidsConfig config;
    config.intersection_depth = 2;
    const auto decide = [&](const CsidsConfig& with, Seed seed, IntersectionCounters& counters,
                            DecisionPhase phase = DecisionPhase::random) {
        Csids procedure(repr, num_vars, seed, with);
        procedure.set_decision_phase(phase);
        for (ClauseId id = 0; id < clauses.size(); ++id) {
            procedure.update_with_new_clause(clauses[id], id);
        }
        procedure.update_with_resolved_clause(0);
        const std::vector<std::size_t> stamps{1};
        const DecisionResult result =
            procedure.make_decision(empty, clauses, active, SatisfactionRecord(stamps, 0));
        REQUIRE(result.equation.has_value());
        CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{0});
        counters = procedure.intersection_counters();
        return bits_of(*result.equation);
    };

    SUBCASE("the plain form keeps nothing") {
        IntersectionCounters counters;
        (void)decide(config, default_seed, counters);
        CHECK(counters.decisions == 0);
        CHECK(counters.empty_intersections == 1);
    }

    SUBCASE("span^± keeps the shared direction, on either side") {
        config.span = CsidsSpan::plus_minus;
        std::size_t positive = 0;
        std::size_t negative = 0;
        for (std::uint64_t seed = 0; seed < 32; ++seed) {
            IntersectionCounters counters;
            const std::vector<std::size_t> bits = decide(config, Seed{seed}, counters);
            CHECK(counters.decisions == 1);
            CHECK(counters.intersected_clauses == 1);
            CHECK(counters.dimension_before == 2);
            CHECK(counters.dimension_after == 1);
            CHECK(counters.dimension_one == 1);
            if (bits == std::vector<std::size_t>{0}) {
                ++positive;
            } else {
                CHECK(bits == std::vector<std::size_t>{0, num_vars});
                ++negative;
            }
        }
        CHECK(positive > 0);
        CHECK(negative > 0);
    }

    SUBCASE("span^± puts falsify on the top clause's side, and satisfy on the other") {
        config.span = CsidsSpan::plus_minus;
        for (std::uint64_t seed = 0; seed < 8; ++seed) {
            IntersectionCounters counters;
            CHECK(decide(config, Seed{seed}, counters, DecisionPhase::falsify) ==
                  std::vector<std::size_t>{0});
            CHECK(decide(config, Seed{seed}, counters, DecisionPhase::satisfy) ==
                  std::vector<std::size_t>{0, num_vars});
        }
    }

    SUBCASE("rotate puts a side no trail decides on the falsify side") {
        config.phase_saving.rotation_interval = 1000;
        for (const CsidsSpan span : {CsidsSpan::plain, CsidsSpan::plus_minus}) {
            config.span = span;
            for (std::uint64_t seed = 0; seed < 16; ++seed) {
                IntersectionCounters counters;
                const std::vector<std::size_t> bits =
                    decide(config, Seed{seed}, counters, DecisionPhase::rotate);
                // The falsify side lies in the top clause's span(x0, x1): no constant.
                CHECK(!bits.empty());
                CHECK(std::ranges::find(bits, num_vars) == bits.end());
                if (span == CsidsSpan::plus_minus) {
                    CHECK(bits == std::vector<std::size_t>{0});
                }
            }
        }
    }
}

TEST_CASE("phase rotation: every step of cycle c lasts c intervals") {
    // Interval 2: four steps of 2 conflicts, then four of 4, then four of 6.
    PhaseRotation rotation(2);
    const PhaseOrder cycle[]{PhaseOrder::target_recent_falsify, PhaseOrder::falsify,
                             PhaseOrder::target_falsify, PhaseOrder::recent_falsify};
    std::vector<PhaseOrder> expected;
    for (std::size_t c = 1; c <= 3; ++c) {
        for (const PhaseOrder order : cycle) {
            expected.insert(expected.end(), 2 * c, order);
        }
    }
    std::size_t rotations = 0;
    for (std::size_t conflicts = 0; conflicts < expected.size(); ++conflicts) {
        CHECK(rotation.order() == expected[conflicts]);
        if (rotation.update_with_conflict()) {
            ++rotations;
        }
    }
    CHECK(rotations == 12);
    CHECK(rotation.order() == PhaseOrder::target_recent_falsify);
}

TEST_CASE_TEMPLATE("saved trails: the clause step chooses a side consistent with the trail",
                   Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    constexpr std::size_t num_vars = 4;
    const PhaseSavingConfig config{
        .recent_trails = 1, .target = false, .rotation_interval = std::nullopt};
    SavedTrails saved(repr, num_vars, config);

    // The saved trail S: `x0 = 0` decided at level 1, then `x1 = 0` propagated, then
    // `0 = 1`, the conflict, which S leaves out. The search goes back to the root and decides `x0 = 1` instead, so U = span(x0 = 1).
    Trail current(repr, num_vars);
    current.learn(make_equation_with(repr, num_vars, {0}), std::nullopt, 1);
    current.learn(make_equation_with(repr, num_vars, {1}), ClauseId{0}, 1);
    current.learn(make_equation_with(repr, num_vars, {num_vars}), ClauseId{1}, 1);
    saved.update_with_conflict(current, std::vector<std::size_t>{1, 2});
    current.resize(1);
    current.learn(make_equation_with(repr, num_vars, {0, num_vars}), std::nullopt, 1);
    const std::vector<std::size_t> stamps{1, 3};

    // `neg(C)` mentions x0, which S and U set apart; `neg(D)` does not.
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{0, num_vars}, {1}, {2}}));
    clauses.push_back(make_clause(repr, num_vars, {{1}, {3}}));
    const auto space_of = [&](const std::vector<std::vector<std::size_t>>& rows) {
        RowSpace space(repr, num_vars);
        for (const std::vector<std::size_t>& bits : rows) {
            space.add_row(Equation::from_bits(repr, num_vars, std::span<const std::size_t>(bits)));
        }
        return space;
    };

    SUBCASE("a clause not mentioning x0 decides x1 = 0") {
        RowSpace space = space_of({{1}, {3}});
        saved.narrow(space, std::vector<ClauseId>{1}, clauses, current.span(), stamps);
        REQUIRE(space.rows() == 1);
        CHECK(bits_of(space.row(0)) == std::vector<std::size_t>{1});
        CHECK(saved.counters().recent_clause_step.accepted == 1);
        CHECK(saved.counters().clause_step_top_clause == 1);
        CHECK(saved.counters().uniform_sides == 0);
        CHECK(saved.counters().dimension_before == 2);
        CHECK(saved.counters().dimension_after == 1);
    }

    SUBCASE("a clause mentioning x0 holds both sides of x1, and narrowing leaves the side open") {
        // `x0 = 0` and `x0 + x1 = 0` of S are `0 = 1` and `x1 = 1` modulo U.
        RowSpace space = space_of({{1}, {2}});
        saved.narrow(space, std::vector<ClauseId>{0}, clauses, current.span(), stamps);
        CHECK(saved.counters().recent_clause_step.inconsistent == 1);
        CHECK(saved.counters().recent_clause_step.accepted == 0);
        CHECK(saved.counters().recent_narrowing == 1);
        REQUIRE(space.rows() == 2);
        CHECK(bits_of(space.row(0)) == std::vector<std::size_t>{1});
        CHECK(bits_of(space.row(1)) == std::vector<std::size_t>{num_vars});
        CHECK(saved.counters().uniform_sides == 1);
    }

    SUBCASE("past an inconsistent top clause a partner decides") {
        RowSpace space = space_of({{1}});
        saved.narrow(space, std::vector<ClauseId>{0, 1}, clauses, current.span(), stamps);
        REQUIRE(space.rows() == 1);
        CHECK(bits_of(space.row(0)) == std::vector<std::size_t>{1});
        CHECK(saved.counters().recent_clause_step.inconsistent == 1);
        CHECK(saved.counters().clause_step_partner == 1);
    }
}

TEST_CASE_TEMPLATE("saved trails: focused mode consults no target trail", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    constexpr std::size_t num_vars = 4;

    // A conflict at level 2 makes level 1, `x0 = 0`, the target; the search is back at the root.
    Trail current(repr, num_vars);
    current.learn(make_equation_with(repr, num_vars, {0}), std::nullopt, 1);
    current.learn(make_equation_with(repr, num_vars, {1}), std::nullopt, 2);
    current.learn(make_equation_with(repr, num_vars, {num_vars}), ClauseId{0}, 2);
    const std::vector<std::size_t> stamps{1};
    const std::vector<Clause> clauses;
    const auto narrowed = [&](SavedTrails& saved) {
        saved.update_with_conflict(current, std::vector<std::size_t>{1, 2, 3});
        current.resize(1);
        RowSpace space(repr, num_vars);
        space.add_row(make_equation_with(repr, num_vars, {0}));
        space.add_row(make_equation_with(repr, num_vars, {1}));
        saved.narrow(space, std::vector<ClauseId>{}, clauses, current.span(), stamps);
        return space;
    };
    const PhaseSavingConfig config{
        .recent_trails = 0, .target = true, .rotation_interval = std::nullopt};

    SUBCASE("stable mode narrows to the target's side") {
        SavedTrails saved(repr, num_vars, config);
        const RowSpace space = narrowed(saved);
        REQUIRE(space.rows() == 1);
        CHECK(bits_of(space.row(0)) == std::vector<std::size_t>{0});
        CHECK(saved.counters().target_narrowing == 1);
    }

    SUBCASE("focused mode leaves the side open") {
        SavedTrails saved(repr, num_vars, config);
        saved.update_with_mode(SearchMode::focused);
        const RowSpace space = narrowed(saved);
        CHECK(space.rows() == 3);
        CHECK(saved.counters().target_narrowing == 0);
        CHECK(saved.counters().none == 1);
        CHECK(saved.counters().uniform_sides == 1);
    }

    SUBCASE("under rotation, focused mode's target orders consult no trail") {
        PhaseSavingConfig rotating = config;
        rotating.rotation_interval = 1000;
        SavedTrails saved(repr, num_vars, rotating);
        CHECK(saved.consults_trails());
        saved.update_with_mode(SearchMode::focused);
        CHECK(!saved.consults_trails());
        rotating.recent_trails = 1;
        SavedTrails with_recent(repr, num_vars, rotating);
        with_recent.update_with_mode(SearchMode::focused);
        CHECK(with_recent.consults_trails());
    }
}

} // namespace xorcle
