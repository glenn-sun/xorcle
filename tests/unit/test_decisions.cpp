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
#include "heuristics/decisions/phase.h"
#include "heuristics/decisions/saved_trails.h"
#include "heuristics_test_support.h"
#include "util/index_set.h"
#include "util/random.h"

namespace xorcle {

TEST_CASE_TEMPLATE("phase: applying a decision phase", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Rng rng = make_rng(3, RngComponent::decision_phase);

    SUBCASE("falsify leaves the equation alone") {
        Equation eqn = make_equation_with(repr, 4, {1});
        apply_phase(eqn, DecisionPhase::falsify, rng);
        CHECK(bits_of(eqn) == std::vector<std::size_t>{1});
    }

    SUBCASE("satisfy flips the constant column") {
        Equation eqn = make_equation_with(repr, 4, {1});
        apply_phase(eqn, DecisionPhase::satisfy, rng);
        CHECK(bits_of(eqn) == std::vector<std::size_t>{1, 4});

        apply_phase(eqn, DecisionPhase::satisfy, rng);
        CHECK(bits_of(eqn) == std::vector<std::size_t>{1});
    }

    SUBCASE("random is reproducible from the seed and only ever flips the constant") {
        Rng other = make_rng(3, RngComponent::decision_phase);
        for (int draw = 0; draw < 16; ++draw) {
            Equation first = make_equation_with(repr, 4, {1});
            Equation second = make_equation_with(repr, 4, {1});
            apply_phase(first, DecisionPhase::random, rng);
            apply_phase(second, DecisionPhase::random, other);
            CHECK(first == second);
            const std::vector<std::size_t> bits = bits_of(first);
            CHECK((bits == std::vector<std::size_t>{1} ||
                   bits == std::vector<std::size_t>{1, 4}));
        }
    }
}

TEST_CASE_TEMPLATE("csids: activity drives the clause choice", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Csids procedure(repr, 4, default_seed, csids_config_deciding(DecisionPhase::falsify));
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
    }

    SUBCASE("a new asserting clause enters above every starting clause") {
        procedure.update_with_asserting_clause(clauses[2]);
        procedure.update_with_new_clause(clauses[2], 2);
        CHECK(procedure.make_decision(empty, clauses, active).equation.has_value());
        CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{2});
    }

    SUBCASE("the satisfy phase flips the sampled equation's constant column") {
        Csids satisfying(repr, 4, default_seed, csids_config_deciding(DecisionPhase::satisfy));
        satisfying.update_with_new_clause(clauses[0], 0);
        satisfying.update_with_new_clause(clauses[1], 1);

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
        std::vector<ClauseRecord> records(clauses.size());
        records[0].satisfied_level = 0;
        records[0].satisfied_stamp = 1;
        CHECK(procedure
                  .make_decision(empty, clauses, active, SatisfactionRecord(records, stamps, 0))
                  .equation.has_value());
        CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{1});

        SUBCASE("it stays off the heap while that level stands") {
            procedure.update_with_backjump(0);
            CHECK(procedure
                      .make_decision(empty, clauses, active, SatisfactionRecord(records, stamps, 0))
                      .equation.has_value());
            CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{1});
        }
    }

    SUBCASE("a record whose level has since been replaced is not trusted") {
        procedure.update_with_resolved_clause(0);
        const std::vector<std::size_t> stamps{7};
        std::vector<ClauseRecord> records(clauses.size());
        records[0].satisfied_level = 0;
        records[0].satisfied_stamp = 1;
        CHECK(procedure
                  .make_decision(empty, clauses, active, SatisfactionRecord(records, stamps, 0))
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
        Csids fallback(repr, 4, default_seed, default_csids_config());
        const std::vector<Clause> no_clauses;
        const std::vector<std::uint8_t> no_active;
        // Every active clause is satisfied, so the solver completes the model
        // rather than deciding the rest one variable at a time.
        CHECK_FALSE(fallback.make_decision(empty, no_clauses, no_active).equation.has_value());
        CHECK(fallback.decision_clause_hint() == std::nullopt);
        CHECK(fallback.counters().decisions == 0);
    }

    SUBCASE("reinitialize restores the sampling stream") {
        procedure.update_with_resolved_clause(1);
        const Equation first = *procedure.make_decision(empty, clauses, active).equation;
        procedure.reinitialize(); // Keeps the registered clauses, at activity 0.
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
        procedure.update_with_substitution(empty, dropped, clauses, clause_active);
        CHECK(procedure.num_vars() == 4);
        CHECK(procedure.decision_clause_hint() == std::nullopt);

        procedure.update_with_renumbering(transform);
        CHECK(procedure.num_vars() == 3);
    }
}

TEST_CASE_TEMPLATE("csids: decay makes later bumps worth more", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    Csids procedure(repr, 4, default_seed, csids_config_deciding(DecisionPhase::falsify));
    const RowSpace empty(repr, 4);

    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, 4, {{0}, {1}}));
    clauses.push_back(make_clause(repr, 4, {{1}, {2}}));
    clauses.push_back(make_clause(repr, 4, {{2}, {3}}));
    const std::vector<std::uint8_t> active(clauses.size(), 1);
    procedure.update_with_new_clause(clauses[0], 0);
    procedure.update_with_new_clause(clauses[1], 1);

    // Rather than decaying every score per conflict, the scale grows by
    // 1/decay, so of two bumps either side of a conflict the later is worth
    // strictly more.
    procedure.update_with_resolved_clause(0);
    procedure.update_with_asserting_clause(clauses[2]);
    procedure.update_with_new_clause(clauses[2], 2);
    procedure.update_with_resolved_clause(1);

    CHECK(procedure.make_decision(empty, clauses, active).equation.has_value());
    CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{1});
}

TEST_CASE_TEMPLATE("csids: with A_0 = 1 and no bump, learned clauses come newest first", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    CsidsConfig config = default_csids_config();
    config.initial_activity = 1.0;
    config.bump = 0.0;
    config.intersection_depth = 1;
    config.phase_saving.source = PhaseSaving::none;
    Csids procedure(repr, 4, default_seed, config);
    const RowSpace empty(repr, 4);

    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, 4, {{0}, {1}}));
    clauses.push_back(make_clause(repr, 4, {{1}, {2}}));
    clauses.push_back(make_clause(repr, 4, {{2}, {3}}));
    clauses.push_back(make_clause(repr, 4, {{0}, {2}}));
    clauses.push_back(make_clause(repr, 4, {{1}, {3}}));
    std::vector<std::uint8_t> active(clauses.size(), 1);
    procedure.update_with_new_clause(clauses[0], 0);
    procedure.update_with_new_clause(clauses[1], 1);
    for (ClauseId id = 2; id < clauses.size(); ++id) {
        procedure.update_with_asserting_clause(clauses[id]);
        procedure.update_with_new_clause(clauses[id], id);
        // Resolving through an older clause adds nothing.
        procedure.update_with_resolved_clause(id - 2);
    }

    for (ClauseId expected = 4; expected >= 2; --expected) {
        CHECK(procedure.make_decision(empty, clauses, active).equation.has_value());
        CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{expected});
        active[expected] = 0;
    }
}

TEST_CASE_TEMPLATE("csids: a deeper intersection narrows the top clause's decision", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    CsidsConfig config = default_csids_config();
    config.intersection_depth = 2;
    config.phase_saving.source = PhaseSaving::none;
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
        // The top clause alone draws from x0, x1 and x0 + x1.
        for (std::uint64_t seed = 0; seed < 16; ++seed) {
            Csids seeded(repr, 4, Seed{seed}, config);
            for (ClauseId id = 0; id < clauses.size(); ++id) {
                seeded.update_with_new_clause(clauses[id], id);
            }
            seeded.update_with_resolved_clause(0);
            const DecisionResult result = seeded.make_decision(empty, clauses, active);
            REQUIRE(result.equation.has_value());
            CHECK(bits_of(*result.equation) == std::vector<std::size_t>{1});
            CHECK(seeded.decision_clause_hint() == std::optional<ClauseId>{0});
        }
    }

    SUBCASE("a clause sharing no pivot column is passed over") {
        config.intersection_depth = 3;
        Csids deeper(repr, 4, default_seed, config);
        for (ClauseId id = 0; id < clauses.size(); ++id) {
            deeper.update_with_new_clause(clauses[id], id);
        }
        deeper.update_with_resolved_clause(0);
        deeper.update_with_resolved_clause(0);
        deeper.update_with_resolved_clause(2);
        const DecisionResult result = deeper.make_decision(empty, clauses, active);
        REQUIRE(result.equation.has_value());
        CHECK(bits_of(*result.equation) == std::vector<std::size_t>{1});
    }

    SUBCASE("with nothing kept the draw is the top clause's alone") {
        CsidsConfig top_alone = default_csids_config();
        top_alone.intersection_depth = 1;
        top_alone.phase_saving.source = PhaseSaving::none;
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
    }

    SUBCASE("a propagating clause below the top is handed back") {
        procedure.update_with_resolved_clause(0);
        const RowSpace span = make_row_space(repr, 4, {{2}});
        const DecisionResult result = procedure.make_decision(span, clauses, active);
        CHECK(!result.equation.has_value());
        CHECK(result.propagating_clause == std::optional<ClauseId>{1});

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

    CsidsConfig config = default_csids_config();
    config.intersection_depth = 2;
    config.phase_saving.source = PhaseSaving::none;
    const auto decide = [&](const CsidsConfig& with, Seed seed,
                            DecisionPhase phase = DecisionPhase::random) {
        CsidsConfig phased = with;
        phased.phase = phase;
        Csids procedure(repr, num_vars, seed, phased);
        for (ClauseId id = 0; id < clauses.size(); ++id) {
            procedure.update_with_new_clause(clauses[id], id);
        }
        procedure.update_with_resolved_clause(0);
        const std::vector<std::size_t> stamps{1};
        const std::vector<ClauseRecord> records(clauses.size());
        const DecisionResult result = procedure.make_decision(
            empty, clauses, active, SatisfactionRecord(records, stamps, 0));
        REQUIRE(result.equation.has_value());
        CHECK(procedure.decision_clause_hint() == std::optional<ClauseId>{0});
        return bits_of(*result.equation);
    };

    SUBCASE("the plain form keeps nothing: it draws as the top clause alone does") {
        CsidsConfig top_alone = config;
        top_alone.intersection_depth = 1;
        for (std::uint64_t seed = 0; seed < 16; ++seed) {
            CHECK(decide(config, Seed{seed}) == decide(top_alone, Seed{seed}));
        }
    }

    SUBCASE("span^± keeps the shared direction, on either side") {
        config.span = CsidsSpan::plus_minus;
        std::size_t positive = 0;
        std::size_t negative = 0;
        for (std::uint64_t seed = 0; seed < 32; ++seed) {
            const std::vector<std::size_t> bits = decide(config, Seed{seed});
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
            CHECK(decide(config, Seed{seed}, DecisionPhase::falsify) ==
                  std::vector<std::size_t>{0});
            CHECK(decide(config, Seed{seed}, DecisionPhase::satisfy) ==
                  std::vector<std::size_t>{0, num_vars});
        }
    }

    SUBCASE("rotate puts a side no trail decides on the falsify side") {
        config.phase_saving.source = PhaseSaving::rotate;
        for (const CsidsSpan span : {CsidsSpan::plain, CsidsSpan::plus_minus}) {
            config.span = span;
            for (std::uint64_t seed = 0; seed < 16; ++seed) {
                const std::vector<std::size_t> bits =
                    decide(config, Seed{seed}, DecisionPhase::falsify);
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
    const PhaseSaving cycle[]{PhaseSaving::both, PhaseSaving::none, PhaseSaving::target_only,
                              PhaseSaving::recent_only};
    std::vector<PhaseSaving> expected;
    for (std::size_t c = 1; c <= 3; ++c) {
        for (const PhaseSaving source : cycle) {
            expected.insert(expected.end(), 2 * c, source);
        }
    }
    std::size_t rotations = 0;
    for (std::size_t conflicts = 0; conflicts < expected.size(); ++conflicts) {
        CHECK(rotation.source() == expected[conflicts]);
        if (rotation.update_with_conflict()) {
            ++rotations;
        }
    }
    CHECK(rotations == 12);
    CHECK(rotation.source() == PhaseSaving::both);
}

TEST_CASE_TEMPLATE("saved trails: the clause step chooses a side consistent with the trail",
                   Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    constexpr std::size_t num_vars = 4;
    const PhaseSavingConfig config{.source = PhaseSaving::recent_only,
                                   .target_reset_conflicts = 1000,
                                   .rotate_starting_interval = 1000,
                                   .focused_source = std::nullopt};
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
        CHECK(saved.counters().phase_default_decisions == 0);
    }

    SUBCASE("a clause mentioning x0 holds both sides of x1, and narrowing leaves the side open") {
        // `x0 = 0` and `x0 + x1 = 0` of S are `0 = 1` and `x1 = 1` modulo U.
        RowSpace space = space_of({{1}, {2}});
        saved.narrow(space, std::vector<ClauseId>{0}, clauses, current.span(), stamps);
        REQUIRE(space.rows() == 2);
        CHECK(bits_of(space.row(0)) == std::vector<std::size_t>{1});
        CHECK(bits_of(space.row(1)) == std::vector<std::size_t>{num_vars});
        CHECK(saved.counters().phase_default_decisions == 1);
    }

    SUBCASE("past an inconsistent top clause a partner decides") {
        RowSpace space = space_of({{1}});
        saved.narrow(space, std::vector<ClauseId>{0, 1}, clauses, current.span(), stamps);
        REQUIRE(space.rows() == 1);
        CHECK(bits_of(space.row(0)) == std::vector<std::size_t>{1});
    }
}

TEST_CASE_TEMPLATE("saved trails: each mode consults its own source", Tag, DenseTag, SparseTag) {
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
    const PhaseSavingConfig config{.source = PhaseSaving::target_only,
                                   .target_reset_conflicts = 1000,
                                   .rotate_starting_interval = 1000,
                                   .focused_source = PhaseSaving::recent_only};

    SUBCASE("stable mode narrows to the target's side") {
        SavedTrails saved(repr, num_vars, config);
        const RowSpace space = narrowed(saved);
        REQUIRE(space.rows() == 1);
        CHECK(bits_of(space.row(0)) == std::vector<std::size_t>{0});
    }

    SUBCASE("focused mode narrows by the recent trail") {
        SavedTrails saved(repr, num_vars, config);
        saved.update_with_mode(SearchMode::focused);
        CHECK(saved.consults_trails());
        const RowSpace space = narrowed(saved);
        // Both of the recent trail's levels, where the target holds only the first.
        REQUIRE(space.rows() == 2);
        CHECK(bits_of(space.row(0)) == std::vector<std::size_t>{0});
        CHECK(bits_of(space.row(1)) == std::vector<std::size_t>{1});
        CHECK(saved.counters().phase_default_decisions == 0);
    }

    SUBCASE("a mode whose source is none consults no trail") {
        PhaseSavingConfig none_focused = config;
        none_focused.focused_source = PhaseSaving::none;
        SavedTrails saved(repr, num_vars, none_focused);
        CHECK(saved.consults_trails());
        saved.update_with_mode(SearchMode::focused);
        CHECK(!saved.consults_trails());
        saved.note_unconsulted_decision();
        CHECK(saved.counters().phase_default_decisions == 1);
    }
}

} // namespace xorcle
