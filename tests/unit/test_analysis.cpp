#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra_test_support.h"
#include "core/analysis.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "core/trail_basis.h"
#include "heuristics/decisions/decision_procedure.h"
#include "heuristics_test_support.h"
#include "util/random.h"

namespace xorcle {

namespace {

/// @brief A decision procedure that only records, in order, the notifications
/// conflict analysis makes.
class RecordingDecisions final : public DecisionProcedure {
public:
    struct Event {
        enum class Kind {
            resolved, ///< `update_with_resolved_clause()`
            critical ///< `update_with_critical_equation()`
        };

        Kind kind;
        ClauseId id = 0; ///< The reason clause, for `resolved`.
        std::vector<std::size_t> bits; ///< The equation's set bits, for `critical`.

        bool operator==(const Event& other) const = default;
    };

    RecordingDecisions(EquationRepr repr, std::size_t num_vars)
        : DecisionProcedure(repr, num_vars, Seed{0}) {}

    /// @brief Unused; conflict analysis never resets a decision procedure.
    void reinitialize() override {}

    /// @brief Unused; conflict analysis never decides.
    Equation make_decision(const RowSpace&, const std::vector<Clause>&,
                           const std::vector<std::uint8_t>&) override {
        CHECK_MESSAGE(false, "conflict analysis must not make decisions");
        return Equation(repr(), num_vars());
    }

    void update_with_resolved_clause(ClauseId id) override {
        events.push_back(Event{Event::Kind::resolved, id, {}});
    }

    void update_with_critical_equation(const Equation& eqn) override {
        events.push_back(Event{Event::Kind::critical, 0, bits_of(eqn)});
    }

    /// @brief The notifications received, oldest first.
    std::vector<Event> events;
};

/// @brief A `resolved` event, for building an expected sequence.
RecordingDecisions::Event resolved(ClauseId id) {
    return RecordingDecisions::Event{RecordingDecisions::Event::Kind::resolved, id, {}};
}

/// @brief A `critical` event, for building an expected sequence.
RecordingDecisions::Event critical(std::vector<std::size_t> bits) {
    return RecordingDecisions::Event{RecordingDecisions::Event::Kind::critical, 0,
                                     std::move(bits)};
}

/// @brief The rows of `clause` that do not lie in `span`; the clause is
/// asserting exactly when this is 1.
std::size_t rows_outside_span(const Clause& clause, const RowSpace& span) {
    std::size_t outside = 0;
    for (const Equation& row : clause) {
        Equation reduced = row;
        span.reduce(reduced);
        if (!reduced.is_eq_true()) {
            ++outside;
        }
    }
    return outside;
}

/// @brief Check the learned clause asserts at `learned.asserting_level` and at
/// no lower level, by backjumping a copy of the trail.
void check_asserts_at(const AssertingClause& learned, const Trail& trail) {
    Trail backjumped = trail;
    backjumped.resize(learned.asserting_level + 1);
    CHECK(rows_outside_span(learned.clause, backjumped.span()) == 1);

    if (learned.asserting_level > 0) {
        Trail lower = trail;
        lower.resize(learned.asserting_level);
        CHECK(rows_outside_span(learned.clause, lower.span()) >= 2);
    }
}

/// @brief The rows of a clause or row space, as lists of set bits.
std::vector<std::vector<std::size_t>> rows_of(const RowEchelonMatrix& matrix) {
    std::vector<std::vector<std::size_t>> rows;
    for (const Equation& row : matrix) {
        rows.push_back(bits_of(row));
    }
    return rows;
}

/// @brief A conflicting trail together with the clause database that built it.
struct Conflict {
    Trail trail;
    std::vector<Clause> clauses; ///< The clause database, in clause-id order.
};

/// @brief The smallest conflict that needs exactly one resolution step: three
/// variables, one decision level, and a chain of two propagations closed by a
/// conflicting clause.
///
/// | step | equation | reason | level |
/// |---|---|---|---|
/// | 0 | `[x0 = 0]` | decision | 1 |
/// | 1 | `[x1 = 1]` | clause 0 | 1 |
/// | 2 | `[x2 = 1]` | clause 1 | 1 |
/// | 3 | `0 = 1` | clause 2 | 1 |
///
/// Clause 2 is `[x1 = 0] v [x2 = 0]`, both equations falsified. Every equation
/// is learned in the form the solver learns it in -- already reduced by the
/// span -- so step 3 is `0 = 1` rather than the `[x1 = 0]` clause 2 implied.
Conflict make_one_step_conflict(EquationRepr repr) {
    constexpr std::size_t num_vars = 3;
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{0}, {1}})); // 0: implies [x1 = 1]
    clauses.push_back(make_clause(repr, num_vars, {{1, 3}, {2}})); // 1: implies [x2 = 1]
    clauses.push_back(make_clause(repr, num_vars, {{1, 3}, {2, 3}})); // 2: the conflict

    Trail trail(repr, num_vars);
    trail.learn(make_equation_with(repr, num_vars, {0}), std::nullopt);
    trail.learn(make_equation_with(repr, num_vars, {1, 3}), ClauseId{0});
    trail.learn(make_equation_with(repr, num_vars, {2, 3}), ClauseId{1});
    trail.learn(Equation::eq_false(repr, num_vars), ClauseId{2});
    return Conflict{std::move(trail), std::move(clauses)};
}

/// @brief A conflict over two decision levels, needing two resolution steps.
///
/// Six variables: level 1 fixes `[x5 = 1]`, level 2 derives a chain
/// x2 -> x3, x4 that the conflicting clause closes.
///
/// | step | equation | reason | level | span row | basis column |
/// |---|---|---|---|---|---|
/// | 0 | `[x0 = 0]` | decision | 1 | 0 | 6 |
/// | 1 | `[x5 = 1]` | clause 0 | 1 | 5 | 5 |
/// | 2 | `[x1 = 0]` | decision | 2 | 1 | 4 |
/// | 3 | `[x2 = 1]` | clause 1 | 2 | 2 | 3 |
/// | 4 | `[x3 = 1]` | clause 2 | 2 | 3 | 2 |
/// | 5 | `[x4 = 1]` | clause 3 | 2 | 4 | 1 |
/// | 6 | `0 = 1` | `conflict_clause_id` | 2 | 6 | 0 |
///
/// Row-echelon order and trail order deliberately disagree: step 1 lands at
/// span row 5. `conflict_clause_id` picks clause 4, whose analysis takes two
/// resolution steps, or clause 5, which is already asserting.
Conflict make_two_level_conflict(EquationRepr repr, ClauseId conflict_clause_id) {
    constexpr std::size_t num_vars = 6;
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{0}, {5}})); // 0: implies [x5 = 1]
    clauses.push_back(make_clause(repr, num_vars, {{1}, {2}})); // 1: implies [x2 = 1]
    clauses.push_back(make_clause(repr, num_vars, {{2, 6}, {3}})); // 2: implies [x3 = 1]
    clauses.push_back(make_clause(repr, num_vars, {{2, 6}, {4}})); // 3: implies [x4 = 1]
    // 4: [x3 + x4 = 1] v [x2 = 0] v [x5 = 0], every literal falsified.
    clauses.push_back(make_clause(repr, num_vars, {{3, 4}, {2, 6}, {5, 6}}));
    // 5: [x3 + x4 = 1] v [x5 = 0], also falsified, but already asserting.
    clauses.push_back(make_clause(repr, num_vars, {{3, 4}, {5, 6}}));

    Trail trail(repr, num_vars);
    trail.learn(make_equation_with(repr, num_vars, {0}), std::nullopt);
    trail.learn(make_equation_with(repr, num_vars, {5, 6}), ClauseId{0});
    trail.learn(make_equation_with(repr, num_vars, {1}), std::nullopt);
    trail.learn(make_equation_with(repr, num_vars, {2, 6}), ClauseId{1});
    trail.learn(make_equation_with(repr, num_vars, {3, 6}), ClauseId{2});
    trail.learn(make_equation_with(repr, num_vars, {4, 6}), ClauseId{3});
    trail.learn(Equation::eq_false(repr, num_vars), conflict_clause_id);
    return Conflict{std::move(trail), std::move(clauses)};
}

/// @brief A conflict whose learned clause is the level-2 decision negated, so
/// its one pivot is a decision row rather than a propagation.
///
/// | step | equation | reason | level |
/// |---|---|---|---|
/// | 0 | `[x0 = 0]` | decision | 1 |
/// | 1 | `[x1 = 0]` | decision | 2 |
/// | 2 | `[x2 = 1]` | clause 0 | 2 |
/// | 3 | `0 = 1` | clause 1 | 2 |
Conflict make_decision_pivot_conflict(EquationRepr repr) {
    constexpr std::size_t num_vars = 3;
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{1}, {2}})); // 0: implies [x2 = 1]
    clauses.push_back(make_clause(repr, num_vars, {{1}, {2, 3}})); // 1: the conflict

    Trail trail(repr, num_vars);
    trail.learn(make_equation_with(repr, num_vars, {0}), std::nullopt);
    trail.learn(make_equation_with(repr, num_vars, {1}), std::nullopt);
    trail.learn(make_equation_with(repr, num_vars, {2, 3}), ClauseId{0});
    trail.learn(Equation::eq_false(repr, num_vars), ClauseId{1});
    return Conflict{std::move(trail), std::move(clauses)};
}

/// @brief Assert the shape every fixture's trail is claimed to have.
void check_conflicting(const Conflict& conflict) {
    const Trail& trail = conflict.trail;
    REQUIRE(trail.level() > 0);
    REQUIRE(trail.span().rows() == trail.reasons().size());

    // The conflict: 1 lies in the span, and the row that put it there is the
    // most recent one.
    REQUIRE(bits_of(trail.span().row(trail.span().rows() - 1)) ==
            std::vector<std::size_t>{trail.num_vars()});
    REQUIRE(trail.order_map()[trail.span().rows() - 1] == trail.span().rows() - 1);

    // Every reason clause's rows lie in the span.
    for (const std::optional<ClauseId>& reason : trail.reasons()) {
        if (reason.has_value()) {
            CHECK(rows_outside_span(conflict.clauses[*reason], trail.span()) == 0);
        }
    }
}

/// @brief A conflict at the root, where one root propagation is beside the
/// point: `[x1 = 1]` is propagated but no clause the conflict uses mentions it.
///
/// | step | equation | reason |
/// |---|---|---|
/// | 0 | `[x0 = 1]` | clause 0 |
/// | 1 | `[x1 = 1]` | clause 1 |
/// | 2 | `[x2 = 1]` | clause 2 |
/// | 3 | `0 = 1` | clause 3 |
Conflict make_root_conflict(EquationRepr repr) {
    constexpr std::size_t num_vars = 4;
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{0}})); // 0: the unit [x0 = 1]
    clauses.push_back(make_clause(repr, num_vars, {{1}})); // 1: the unit [x1 = 1]
    clauses.push_back(make_clause(repr, num_vars, {{0, 4}, {2}})); // 2: implies [x2 = 1]
    clauses.push_back(make_clause(repr, num_vars, {{0, 4}, {2, 4}})); // 3: the conflict

    Trail trail(repr, num_vars);
    trail.learn(make_equation_with(repr, num_vars, {0, 4}), ClauseId{0});
    trail.learn(make_equation_with(repr, num_vars, {1, 4}), ClauseId{1});
    trail.learn(make_equation_with(repr, num_vars, {2, 4}), ClauseId{2});
    trail.learn(Equation::eq_false(repr, num_vars), ClauseId{3});
    return Conflict{std::move(trail), std::move(clauses)};
}

/// @brief Replay a reason chain the way `tools/check_lrup_parity.py` does: every
/// reason but the last leaves exactly one distinct nonzero residue, which is
/// deduced together with `[0 = 1]`, and the last one leaves none at all.
bool replays_to_conflict(const std::vector<ClauseId>& reasons, const std::vector<Clause>& clauses,
                         EquationRepr repr, std::size_t num_vars) {
    RowSpace space(repr, num_vars);
    for (std::size_t i = 0; i < reasons.size(); ++i) {
        std::vector<std::vector<std::size_t>> residue_bits;
        Equation residue = Equation::eq_true(repr, num_vars);
        for (const Equation& row : clauses[reasons[i]]) {
            Equation reduced = row;
            space.reduce(reduced);
            if (reduced.is_eq_true()) {
                continue;
            }
            if (std::find(residue_bits.begin(), residue_bits.end(), bits_of(reduced)) ==
                residue_bits.end()) {
                residue_bits.push_back(bits_of(reduced));
                residue = reduced;
            }
        }
        const bool is_last = i + 1 == reasons.size();
        if (residue_bits.empty()) {
            return is_last;
        }
        if (residue_bits.size() > 1 || is_last) {
            return false;
        }
        residue.plus_assign(Equation::eq_false(repr, num_vars));
        space.add_row(residue);
    }
    return false;
}

} // namespace

TEST_CASE_TEMPLATE("analysis: the fixtures really do conflict", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    const Conflict one_step = make_one_step_conflict(repr);
    check_conflicting(one_step);
    CHECK(one_step.trail.level() == 1);
    CHECK(one_step.trail.order_map() == std::vector<std::size_t>{0, 1, 2, 3});

    const Conflict two_level = make_two_level_conflict(repr, ClauseId{4});
    check_conflicting(two_level);
    CHECK(two_level.trail.level() == 2);
    CHECK(two_level.trail.order_map() == std::vector<std::size_t>{0, 5, 1, 2, 3, 4, 6});
    CHECK(two_level.trail[0].span().rows() == 0);
    CHECK(two_level.trail[1].span().rows() == 2);
    CHECK(two_level.trail[2].span().rows() == 7);
}

TEST_CASE_TEMPLATE("analysis: a conflict resolved in one step", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Conflict conflict = make_one_step_conflict(repr);
    RecordingDecisions decisions(repr, conflict.trail.num_vars());

    Rng representative_rng = make_rng(default_seed, RngComponent::minimize_representative);
    const AssertingClause learned =
        find_asserting_clause(conflict.trail, conflict.clauses, decisions, representative_rng);

    // Resolving [x1 = 0] v [x2 = 0] against [x1 = 0] v [x2 = 1] on x2 leaves the
    // unit clause [x1 = 0], stored as its linear negation [x1 = 1] = {1, 3}.
    CHECK(rows_of(learned.clause) == std::vector<std::vector<std::size_t>>{{1, 3}});
    CHECK(learned.clause.cols() == conflict.trail.span().cols());
    CHECK(learned.clause.repr() == repr);

    // A unit clause asserts at the root, so the whole decision level goes.
    CHECK(learned.asserting_level == 0);
    check_asserts_at(learned, conflict.trail);

    // Derivation order: the conflicting clause first, then the one reason.
    CHECK(learned.reason_ids == std::vector<ClauseId>{2, 1});

    // Clause 1 propagated trail step 2, whose span row is [x2 = 1] = {2, 3}.
    CHECK(decisions.events ==
          std::vector<RecordingDecisions::Event>{resolved(1), critical({2, 3})});
}

TEST_CASE_TEMPLATE("analysis: a conflict needing several resolution steps", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Conflict conflict = make_two_level_conflict(repr, ClauseId{4});
    RecordingDecisions decisions(repr, conflict.trail.num_vars());

    Rng representative_rng = make_rng(default_seed, RngComponent::minimize_representative);
    const AssertingClause learned =
        find_asserting_clause(conflict.trail, conflict.clauses, decisions, representative_rng);

    // Clause 4 = [x3 + x4 = 1] v [x2 = 0] v [x5 = 0]. Resolving away x4 (reason
    // clause 3) and then x3 (reason clause 2) leaves [x2 = 0] v [x5 = 0], whose
    // linear negation is the two rows [x2 = 1] and [x5 = 1].
    CHECK(rows_of(learned.clause) == std::vector<std::vector<std::size_t>>{{2, 6}, {5, 6}});

    // [x2 = 1] was derived at level 2 and [x5 = 1] at level 1, so discarding
    // level 2 leaves the clause unit.
    CHECK(learned.asserting_level == 1);
    check_asserts_at(learned, conflict.trail);

    CHECK(learned.reason_ids == std::vector<ClauseId>{4, 3, 2});

    // Clause 3 propagated trail step 5 ([x4 = 1], span row 4); clause 2
    // propagated trail step 4 ([x3 = 1], span row 3).
    CHECK(decisions.events == std::vector<RecordingDecisions::Event>{
                                  resolved(3), critical({4, 6}), resolved(2), critical({3, 6})});
}

TEST_CASE_TEMPLATE("analysis: every reason is measured when asked", Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Conflict conflict = make_two_level_conflict(repr, ClauseId{4});
    RecordingDecisions unmeasured_decisions(repr, conflict.trail.num_vars());
    RecordingDecisions measured_decisions(repr, conflict.trail.num_vars());

    Rng unmeasured_rng = make_rng(default_seed, RngComponent::minimize_representative);
    Rng measured_rng = make_rng(default_seed, RngComponent::minimize_representative);
    const AssertingClause unmeasured = find_asserting_clause(
        conflict.trail, conflict.clauses, unmeasured_decisions, unmeasured_rng, true, false);
    const AssertingClause measured = find_asserting_clause(
        conflict.trail, conflict.clauses, measured_decisions, measured_rng, true, true);

    CHECK(unmeasured.reason_lbds.empty());

    // Level 2 owns basis columns 0 to 4 and level 1 the last two. Clause 4 leads
    // at columns 1, 3 and 5, so it reaches into both levels; the rows of each
    // reason resolved against stay inside level 2.
    CHECK(measured.reason_ids == std::vector<ClauseId>{4, 3, 2});
    CHECK(measured.reason_lbds == std::vector<std::size_t>{2, 1, 1});

    // Measuring is all the flag does.
    CHECK(rows_of(measured.clause) == rows_of(unmeasured.clause));
    CHECK(measured.asserting_level == unmeasured.asserting_level);
    CHECK(measured.lbd == unmeasured.lbd);
    CHECK(measured_decisions.events == unmeasured_decisions.events);
}

TEST_CASE_TEMPLATE("analysis: the learned clause's pivots name trail rows", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;

    SUBCASE("a pivot the trail propagated names the reason that propagated it") {
        const Conflict conflict = make_two_level_conflict(repr, ClauseId{4});
        RecordingDecisions decisions(repr, conflict.trail.num_vars());

        Rng reported_rng = make_rng(default_seed, RngComponent::minimize_representative);
        const AssertingClause reported =
            find_asserting_clause(conflict.trail, conflict.clauses, decisions, reported_rng, true,
                                  false, true);
        RecordingDecisions plain_decisions(repr, conflict.trail.num_vars());
        Rng plain_rng = make_rng(default_seed, RngComponent::minimize_representative);
        const AssertingClause plain = find_asserting_clause(conflict.trail, conflict.clauses,
                                                            plain_decisions, plain_rng);
        CHECK(plain.pivot_trail_idxs.empty());
        CHECK(rows_of(reported.clause) == rows_of(plain.clause));

        // The clause is [x2 = 1] v [x5 = 1], derived at trail steps 3 and 1.
        CHECK(reported.pivot_trail_idxs == std::vector<std::size_t>{3, 1});
        CHECK(conflict.trail.reasons()[3] == std::optional<ClauseId>{1});
        CHECK(conflict.trail.reasons()[1] == std::optional<ClauseId>{0});
    }

    SUBCASE("a pivot at a decision row carries no reason") {
        const Conflict conflict = make_decision_pivot_conflict(repr);
        check_conflicting(conflict);
        RecordingDecisions decisions(repr, conflict.trail.num_vars());

        Rng representative_rng = make_rng(default_seed, RngComponent::minimize_representative);
        const AssertingClause learned =
            find_asserting_clause(conflict.trail, conflict.clauses, decisions, representative_rng,
                                  true, false, true);
        // The unit [x1 = 1], which is trail step 1 negated.
        CHECK(rows_of(learned.clause) == std::vector<std::vector<std::size_t>>{{1}});
        CHECK(learned.pivot_trail_idxs == std::vector<std::size_t>{1});
        CHECK_FALSE(conflict.trail.reasons()[1].has_value());
        // Level 2 opens with that decision, so it sits where level 1 ended.
        CHECK(conflict.trail[1].span().rows() == 1);
    }
}

TEST_CASE_TEMPLATE("analysis: an already-asserting conflict clause is learned unchanged", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Conflict conflict = make_two_level_conflict(repr, ClauseId{5});
    RecordingDecisions decisions(repr, conflict.trail.num_vars());

    Rng representative_rng = make_rng(default_seed, RngComponent::minimize_representative);
    const AssertingClause learned =
        find_asserting_clause(conflict.trail, conflict.clauses, decisions, representative_rng);

    // Clause 5 = [x3 + x4 = 1] v [x5 = 0] is already asserting -- its first row
    // spans two current-level trail rows at once -- so the loop body never runs.
    CHECK(rows_of(learned.clause) == rows_of(conflict.clauses[5]));
    CHECK(learned.asserting_level == 1);
    check_asserts_at(learned, conflict.trail);

    CHECK(learned.reason_ids == std::vector<ClauseId>{5});
    CHECK(decisions.events.empty());
}

TEST_CASE_TEMPLATE("analysis: the one-step resolvent subsumes both operands", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Conflict conflict = make_one_step_conflict(repr);
    RecordingDecisions decisions(repr, conflict.trail.num_vars());

    Rng representative_rng = make_rng(default_seed, RngComponent::minimize_representative);
    const AssertingClause learned =
        find_asserting_clause(conflict.trail, conflict.clauses, decisions, representative_rng, true,
                              false, false, true);

    // The resolvent [x1 = 0] has one row where both operands have two, so it
    // subsumes the conflicting clause 2 and the reason clause 1 alike; the
    // rejected minimization trial against clause 0 then subsumes that too.
    CHECK(rows_of(learned.clause) == std::vector<std::vector<std::size_t>>{{1, 3}});
    REQUIRE(learned.subsumed_reasons.size() == 3);

    // The conflicting clause's record, first: its replacement is materialized.
    const SubsumedReason& conflicting = learned.subsumed_reasons[0];
    CHECK(conflicting.clause_id == 2);
    CHECK(conflicting.subsumed_conflicting_clause);
    REQUIRE(conflicting.replacement.has_value());
    CHECK(rows_of(*conflicting.replacement) == std::vector<std::vector<std::size_t>>{{1, 3}});
    CHECK(conflicting.num_reasons == 2);
    CHECK(conflicting.trail_idx == 3); // The conflict row.
    CHECK(conflicting.lbd == 1);

    // The reason's record: the clause as learned is its replacement, so none is
    // materialized.
    const SubsumedReason& reason = learned.subsumed_reasons[1];
    CHECK(reason.clause_id == 1);
    CHECK(!reason.subsumed_conflicting_clause);
    CHECK(!reason.replacement.has_value());
    CHECK(reason.num_reasons == 2);
    CHECK(reason.trail_idx == 2); // The row clause 1 propagated.

    // The harvested record: clause 0 never entered `reason_ids`, so its chain
    // is one longer, and its replacement [x0 = 0] is materialized.
    const SubsumedReason& harvested = learned.subsumed_reasons[2];
    CHECK(harvested.clause_id == 0);
    CHECK(!harvested.subsumed_conflicting_clause);
    REQUIRE(harvested.replacement.has_value());
    CHECK(rows_of(*harvested.replacement) == std::vector<std::vector<std::size_t>>{{0}});
    CHECK(harvested.num_reasons == 3);
    CHECK(harvested.trail_idx == 1); // The row clause 0 propagated.

    // Asking for no records leaves the walk unchanged and the list empty.
    RecordingDecisions plain_decisions(repr, conflict.trail.num_vars());
    Rng plain_rng = make_rng(default_seed, RngComponent::minimize_representative);
    const AssertingClause plain =
        find_asserting_clause(conflict.trail, conflict.clauses, plain_decisions, plain_rng, true,
                              false, false, false);
    CHECK(plain.subsumed_reasons.empty());
    CHECK(rows_of(plain.clause) == rows_of(learned.clause));
}

/// @brief A conflict whose first resolution subsumes its reason while the walk
/// continues for one more step, so the record keeps a materialized replacement.
///
/// | step | equation | reason | level |
/// |---|---|---|---|
/// | 0 | `[x0 = 0]` | decision | 1 |
/// | 1 | `[x1 = 0]` | decision | 2 |
/// | 2 | `[x2 = 1]` | clause 0 | 2 |
/// | 3 | `[x3 = 1]` | clause 1 | 2 |
/// | 4 | `0 = 1` | clause 2 | 2 |
///
/// Resolving the conflict against clause 1 drops its rank from 3 to 2, the
/// subsumption; the result still reaches two rows of level 2, so a second
/// resolution against clause 0 follows before the clause asserts.
Conflict make_subsumed_reason_conflict(EquationRepr repr) {
    constexpr std::size_t num_vars = 5;
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(repr, num_vars, {{1, 2}, {0}})); // 0: implies [x2 = 1]
    clauses.push_back(make_clause(repr, num_vars, {{2, 3, 5}, {2, 5}, {1}})); // 1: implies [x3 = 1]
    clauses.push_back(make_clause(repr, num_vars, {{3, 5}, {1}})); // 2: the conflict

    Trail trail(repr, num_vars);
    trail.learn(make_equation_with(repr, num_vars, {0}), std::nullopt);
    trail.learn(make_equation_with(repr, num_vars, {1}), std::nullopt);
    trail.learn(make_equation_with(repr, num_vars, {2, 5}), ClauseId{0});
    trail.learn(make_equation_with(repr, num_vars, {3, 5}), ClauseId{1});
    trail.learn(Equation::eq_false(repr, num_vars), ClauseId{2});
    return Conflict{std::move(trail), std::move(clauses)};
}

TEST_CASE_TEMPLATE("analysis: a subsumed reason keeps its replacement when the walk continues",
                   Tag, DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Conflict conflict = make_subsumed_reason_conflict(repr);
    check_conflicting(conflict);
    RecordingDecisions decisions(repr, conflict.trail.num_vars());

    Rng representative_rng = make_rng(default_seed, RngComponent::minimize_representative);
    const AssertingClause learned =
        find_asserting_clause(conflict.trail, conflict.clauses, decisions, representative_rng, true,
                              false, false, true);

    CHECK(rows_of(learned.clause) == std::vector<std::vector<std::size_t>>{{0}, {1}});
    CHECK(learned.asserting_level == 1);
    CHECK(learned.reason_ids == std::vector<ClauseId>{2, 1, 0});

    // Only the first resolution subsumed; the second reason kept its rank, so
    // the record's replacement survives the walk with clause 1 minus its
    // propagating row: [x2 = 1] and [x1 = 0], echelonized.
    REQUIRE(learned.subsumed_reasons.size() == 1);
    const SubsumedReason& record = learned.subsumed_reasons[0];
    CHECK(record.clause_id == 1);
    CHECK(!record.subsumed_conflicting_clause);
    REQUIRE(record.replacement.has_value());
    CHECK(rows_of(*record.replacement) == std::vector<std::vector<std::size_t>>{{1}, {2, 5}});
    CHECK(record.num_reasons == 2);
    CHECK(record.trail_idx == 3);
    CHECK(record.lbd == 1);
}

TEST_CASE_TEMPLATE("analysis: no subsumption is recorded when no rank drops", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Conflict conflict = make_two_level_conflict(repr, ClauseId{4});
    RecordingDecisions decisions(repr, conflict.trail.num_vars());

    Rng representative_rng = make_rng(default_seed, RngComponent::minimize_representative);
    const AssertingClause learned =
        find_asserting_clause(conflict.trail, conflict.clauses, decisions, representative_rng, true,
                              false, false, true);

    // Both resolutions and both minimization trials keep every rank, so the
    // walk records nothing.
    CHECK(learned.subsumed_reasons.empty());
    CHECK(learned.reason_ids == std::vector<ClauseId>{4, 3, 2});
}

TEST_CASE_TEMPLATE("analysis: one resolution step in trail-basis coordinates", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Conflict conflict = make_two_level_conflict(repr, ClauseId{4});
    const Trail& trail = conflict.trail;
    RecordingDecisions decisions(repr, trail.num_vars());

    // Clause 4's three rows, in trail-basis coordinates. Column c is trail step
    // m - 1 - c with m = 7, so:
    //   [x3 + x4 = 1] = span rows 3 + 4 = trail steps 4, 5 -> columns 2, 1
    //   [x2 = 1]      = span row 2      = trail step 3     -> column 3
    //   [x5 = 1]      = span row 5      = trail step 1     -> column 5
    TrailBasisSpace basis = TrailBasisSpace::from_RowSpace(conflict.clauses[4], trail);
    REQUIRE(rows_of(basis) == std::vector<std::vector<std::size_t>>{{1, 2}, {3}, {5}});

    // The coordinate vector of 1: the unit vector at column 0, because the most
    // recently learned trail row is 1 itself on a conflicting trail.
    const Equation trail_basis_one = make_equation_with_num_bits(repr, basis.cols(), {0});
    REQUIRE(trail_basis_one.num_bits() == basis.cols());

    Rng representative_rng = make_rng(default_seed, RngComponent::minimize_representative);
    const ResolvedReason reason = resolve_asserting_basis_with_reason(
        basis, 1, trail, conflict.clauses, decisions, trail_basis_one, representative_rng);

    // Column 1 is trail step 5, propagated by clause 3 = [x2 = 0] v [x4 = 1].
    // Its own basis rows are {0, 1} (the propagated row [x4 = 0], which carries
    // the 1 coordinate) and {3} (the falsified row [x2 = 1]). So
    //
    //   new row = {1, 2} + {0, 1} + {0} = {2},
    //
    // and the reason's remaining row {3} joins the basis.
    CHECK(rows_of(basis) == std::vector<std::vector<std::size_t>>{{2}, {3}, {5}});
    CHECK(reason.clause_id == ClauseId{3});
    CHECK_FALSE(reason.lbd.has_value());
    CHECK(decisions.events ==
          std::vector<RecordingDecisions::Event>{resolved(3), critical({4, 6})});
}

TEST_CASE_TEMPLATE("analysis: the loop runs while the asserting level is the current level", Tag,
                   DenseTag, SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Conflict conflict = make_two_level_conflict(repr, ClauseId{4});
    const Trail& trail = conflict.trail;
    RecordingDecisions decisions(repr, trail.num_vars());

    // Walk the same loop by hand. The condition is
    // `asserting_level() == trail.level()`, not `== 0`: the initial basis
    // asserts at level 2 and the final one at level 1.
    TrailBasisSpace basis = TrailBasisSpace::from_RowSpace(conflict.clauses[4], trail);
    const Equation trail_basis_one = make_equation_with_num_bits(repr, basis.cols(), {0});

    Rng representative_rng = make_rng(default_seed, RngComponent::minimize_representative);
    std::vector<std::size_t> asserting_levels{basis.asserting_level()};
    std::size_t steps = 0;
    while (basis.asserting_level() == trail.level()) {
        const std::optional<std::size_t> pivot_col_idx = basis.row(0).leading_bit();
        REQUIRE(pivot_col_idx.has_value());
        resolve_asserting_basis_with_reason(basis, *pivot_col_idx, trail, conflict.clauses,
                                            decisions, trail_basis_one, representative_rng);
        // Each step strictly advances the pivot column.
        REQUIRE(basis.row(0).leading_bit() > pivot_col_idx);
        asserting_levels.push_back(basis.asserting_level());
        ++steps;
        REQUIRE(steps <= trail.span().rows());
    }

    CHECK(steps == 2);
    CHECK(asserting_levels == std::vector<std::size_t>{2, 2, 1});
}

TEST_CASE_TEMPLATE("analysis: a root conflict names only the reasons it uses", Tag, DenseTag,
                   SparseTag) {
    constexpr EquationRepr repr = Tag::value;
    const Conflict conflict = make_root_conflict(repr);
    REQUIRE(conflict.trail.level() == 0);
    REQUIRE(conflict.trail.span().rows() == 4);

    Rng representative_rng = make_rng(default_seed, RngComponent::minimize_representative);
    const std::vector<ClauseId> reasons =
        find_root_conflict_reasons(conflict.trail, conflict.clauses, representative_rng);

    // Clause 1 propagated [x1 = 1], which nothing in the derivation needs.
    CHECK(reasons == std::vector<ClauseId>{0, 2, 3});
    CHECK(replays_to_conflict(reasons, conflict.clauses, repr, conflict.trail.num_vars()));

    // The whole trail would replay too, so the point is that it is not what the
    // chain names.
    CHECK(replays_to_conflict(std::vector<ClauseId>{0, 1, 2, 3}, conflict.clauses, repr,
                              conflict.trail.num_vars()));
}

} // namespace xorcle
