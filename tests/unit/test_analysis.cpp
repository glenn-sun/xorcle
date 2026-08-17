#include <doctest/doctest.h>

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "algebra/algebra.h"
#include "algebra/equation.h"
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
/// conflict analysis makes. The two calls are order-sensitive and feed CSIDS, so
/// getting them wrong changes conflict counts without changing correctness --
/// the kind of bug a "did we learn something" test never catches.
class RecordingDecisions final : public DecisionProcedure {
public:
    struct Event {
        enum class Kind {
            resolved, ///< `update_with_resolved_clause()`
            critical ///< `update_with_critical_equation()`
        };

        Kind kind;
        ClauseId id = 0; ///< The reason clause, for `resolved`.
        std::vector<std::size_t> columns; ///< The equation's set columns, for `critical`.

        bool operator==(const Event& other) const = default;
    };

    RecordingDecisions(Algebra backend, std::size_t num_vars)
        : DecisionProcedure(backend, num_vars, Seed{0}) {}

    /// @brief Unused; conflict analysis never resets a decision procedure.
    void reinitialize() override {}

    /// @brief Unused; conflict analysis never decides.
    Equation make_decision(const RowSpace&, const std::vector<Clause>&) override {
        CHECK_MESSAGE(false, "conflict analysis must not make decisions");
        return Equation(backend(), num_vars());
    }

    void update_with_resolved_clause(ClauseId id) override {
        events.push_back(Event{Event::Kind::resolved, id, {}});
    }

    void update_with_critical_equation(const Equation& eqn) override {
        events.push_back(Event{Event::Kind::critical, 0, columns_of(eqn)});
    }

    /// @brief The notifications received, oldest first.
    std::vector<Event> events;
};

/// @brief A `resolved` event, for building an expected sequence.
RecordingDecisions::Event resolved(ClauseId id) {
    return RecordingDecisions::Event{RecordingDecisions::Event::Kind::resolved, id, {}};
}

/// @brief A `critical` event, for building an expected sequence.
RecordingDecisions::Event critical(std::vector<std::size_t> columns) {
    return RecordingDecisions::Event{RecordingDecisions::Event::Kind::critical, 0,
                                     std::move(columns)};
}

/// @brief The rows of `clause` that do not lie in `span`. A clause is asserting
/// exactly when this is 1: every row but one is falsified, so the last is
/// implied.
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

/// @brief Check the learned clause really asserts where it says it does,
/// independently of how `asserting_level()` is computed: backjump a copy of the
/// trail and count falsified rows directly. At the asserting level exactly one
/// row survives; one level lower at least two do, or the clause would assert
/// lower still.
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

/// @brief The rows of a clause or row space, as lists of set columns.
std::vector<std::vector<std::size_t>> rows_of(const RowEchelonMatrix& matrix) {
    std::vector<std::vector<std::size_t>> rows;
    for (const Equation& row : matrix) {
        rows.push_back(columns_of(row));
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
/// | 3 | @f$\mathbf{1}@f$ | clause 2 | 1 |
///
/// Clause 2 is `[x1 = 0] v [x2 = 0]`, both literals falsified. Resolving it
/// against clause 1 = `[x1 = 0] v [x2 = 1]` on @f$x_2@f$ gives the unit clause
/// `[x1 = 0]`, which asserts at the root. Every equation is learned in the form
/// the solver learns it in -- already reduced by the span -- so step 3 is
/// @f$\mathbf{1}@f$ rather than the raw `[x1 = 0]` the clause implied.
Conflict make_one_step_conflict(Algebra backend) {
    constexpr std::size_t num_vars = 3;
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(backend, num_vars, {{0}, {1}})); // 0: implies [x1 = 1]
    clauses.push_back(make_clause(backend, num_vars, {{1, 3}, {2}})); // 1: implies [x2 = 1]
    clauses.push_back(make_clause(backend, num_vars, {{1, 3}, {2, 3}})); // 2: the conflict

    Trail trail(backend, num_vars);
    trail.learn(make_equation_with(backend, num_vars, {0}), std::nullopt);
    trail.learn(make_equation_with(backend, num_vars, {1, 3}), ClauseId{0});
    trail.learn(make_equation_with(backend, num_vars, {2, 3}), ClauseId{1});
    trail.learn(Equation::eq_false(backend, num_vars), ClauseId{2});
    return Conflict{std::move(trail), std::move(clauses)};
}

/// @brief A conflict over two decision levels, needing two resolution steps.
/// Six variables: level 1 fixes @f$x_5 = 1@f$, level 2 derives a chain
/// @f$x_2 \to x_3, x_4@f$ that the conflicting clause closes.
///
/// | step | equation | reason | level | span row | basis column |
/// |---|---|---|---|---|---|
/// | 0 | `[x0 = 0]` | decision | 1 | 0 | 6 |
/// | 1 | `[x5 = 1]` | clause 0 | 1 | 5 | 5 |
/// | 2 | `[x1 = 0]` | decision | 2 | 1 | 4 |
/// | 3 | `[x2 = 1]` | clause 1 | 2 | 2 | 3 |
/// | 4 | `[x3 = 1]` | clause 2 | 2 | 3 | 2 |
/// | 5 | `[x4 = 1]` | clause 3 | 2 | 4 | 1 |
/// | 6 | @f$\mathbf{1}@f$ | `conflict_clause_id` | 2 | 6 | 0 |
///
/// Row-echelon order and trail order deliberately disagree (step 1 lands at span
/// row 5), so the column arithmetic really has to go through `order_map()`.
/// `conflict_clause_id` picks clause 4, whose analysis takes two resolution
/// steps, or clause 5, which is already asserting.
Conflict make_two_level_conflict(Algebra backend, ClauseId conflict_clause_id) {
    constexpr std::size_t num_vars = 6;
    std::vector<Clause> clauses;
    clauses.push_back(make_clause(backend, num_vars, {{0}, {5}})); // 0: implies [x5 = 1]
    clauses.push_back(make_clause(backend, num_vars, {{1}, {2}})); // 1: implies [x2 = 1]
    clauses.push_back(make_clause(backend, num_vars, {{2, 6}, {3}})); // 2: implies [x3 = 1]
    clauses.push_back(make_clause(backend, num_vars, {{2, 6}, {4}})); // 3: implies [x4 = 1]
    // 4: [x3 + x4 = 1] v [x2 = 0] v [x5 = 0], every literal falsified.
    clauses.push_back(make_clause(backend, num_vars, {{3, 4}, {2, 6}, {5, 6}}));
    // 5: [x3 + x4 = 1] v [x5 = 0], also falsified, but already asserting.
    clauses.push_back(make_clause(backend, num_vars, {{3, 4}, {5, 6}}));

    Trail trail(backend, num_vars);
    trail.learn(make_equation_with(backend, num_vars, {0}), std::nullopt);
    trail.learn(make_equation_with(backend, num_vars, {5, 6}), ClauseId{0});
    trail.learn(make_equation_with(backend, num_vars, {1}), std::nullopt);
    trail.learn(make_equation_with(backend, num_vars, {2, 6}), ClauseId{1});
    trail.learn(make_equation_with(backend, num_vars, {3, 6}), ClauseId{2});
    trail.learn(make_equation_with(backend, num_vars, {4, 6}), ClauseId{3});
    trail.learn(Equation::eq_false(backend, num_vars), conflict_clause_id);
    return Conflict{std::move(trail), std::move(clauses)};
}

/// @brief Assert the shape every fixture's trail is claimed to have.
void check_conflicting(const Conflict& conflict) {
    const Trail& trail = conflict.trail;
    REQUIRE(trail.level() > 0);
    REQUIRE(trail.span().rows() == trail.reasons().size());

    // The conflict: 1 lies in the span, and the row that put it there is the most
    // recent one. Everything in `analysis.cpp` rests on this.
    REQUIRE(columns_of(trail.span().row(trail.span().rows() - 1)) ==
            std::vector<std::size_t>{trail.num_vars()});
    REQUIRE(trail.order_map()[trail.span().rows() - 1] == trail.span().rows() - 1);

    // Every reason clause's rows lie in the span, which is what lets conflict
    // analysis rewrite them into trail-basis coordinates at all.
    for (const std::optional<ClauseId>& reason : trail.reasons()) {
        if (reason.has_value()) {
            CHECK(rows_outside_span(conflict.clauses[*reason], trail.span()) == 0);
        }
    }
}

} // namespace

TEST_CASE_TEMPLATE("analysis: the fixtures really do conflict", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;

    const Conflict one_step = make_one_step_conflict(backend);
    check_conflicting(one_step);
    CHECK(one_step.trail.level() == 1);
    CHECK(one_step.trail.order_map() == std::vector<std::size_t>{0, 1, 2, 3});

    const Conflict two_level = make_two_level_conflict(backend, ClauseId{4});
    check_conflicting(two_level);
    CHECK(two_level.trail.level() == 2);
    CHECK(two_level.trail.order_map() == std::vector<std::size_t>{0, 5, 1, 2, 3, 4, 6});
    CHECK(two_level.trail[0].span().rows() == 0);
    CHECK(two_level.trail[1].span().rows() == 2);
    CHECK(two_level.trail[2].span().rows() == 7);
}

TEST_CASE_TEMPLATE("analysis: a conflict resolved in one step", Tag, DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    const Conflict conflict = make_one_step_conflict(backend);
    RecordingDecisions decisions(backend, conflict.trail.num_vars());
    std::vector<ClauseId> reason_ids;

    const AssertingClause learned =
        find_asserting_clause(conflict.trail, conflict.clauses, decisions, &reason_ids);

    // Resolving [x1 = 0] v [x2 = 0] against [x1 = 0] v [x2 = 1] on x2 leaves the
    // unit clause [x1 = 0], stored as its linear negation [x1 = 1] = {1, 3}.
    CHECK(rows_of(learned.clause) == std::vector<std::vector<std::size_t>>{{1, 3}});
    CHECK(learned.clause.cols() == conflict.trail.span().cols());
    CHECK(learned.clause.backend() == backend);

    // A unit clause asserts at the root, so the whole decision level goes.
    CHECK(learned.asserting_level == 0);
    check_asserts_at(learned, conflict.trail);

    // Derivation order: the conflicting clause first, then the one reason.
    CHECK(reason_ids == std::vector<ClauseId>{2, 1});

    // Clause 1 propagated trail step 2, whose span row is [x2 = 1] = {2, 3}.
    CHECK(decisions.events ==
          std::vector<RecordingDecisions::Event>{resolved(1), critical({2, 3})});
}

TEST_CASE_TEMPLATE("analysis: a conflict needing several resolution steps", Tag, DenseTag,
                   SparseTag) {
    constexpr Algebra backend = Tag::value;
    const Conflict conflict = make_two_level_conflict(backend, ClauseId{4});
    RecordingDecisions decisions(backend, conflict.trail.num_vars());
    std::vector<ClauseId> reason_ids;

    const AssertingClause learned =
        find_asserting_clause(conflict.trail, conflict.clauses, decisions, &reason_ids);

    // Clause 4 = [x3 + x4 = 1] v [x2 = 0] v [x5 = 0]. Resolving away x4 (reason
    // clause 3) and then x3 (reason clause 2) leaves [x2 = 0] v [x5 = 0], whose
    // linear negation is the two rows [x2 = 1] and [x5 = 1].
    CHECK(rows_of(learned.clause) == std::vector<std::vector<std::size_t>>{{2, 6}, {5, 6}});

    // [x2 = 1] was derived at level 2 and [x5 = 1] at level 1, so discarding
    // level 2 leaves the clause unit.
    CHECK(learned.asserting_level == 1);
    check_asserts_at(learned, conflict.trail);

    CHECK(reason_ids == std::vector<ClauseId>{4, 3, 2});

    // Clause 3 propagated trail step 5 ([x4 = 1], span row 4); clause 2
    // propagated trail step 4 ([x3 = 1], span row 3).
    CHECK(decisions.events == std::vector<RecordingDecisions::Event>{
                                  resolved(3), critical({4, 6}), resolved(2), critical({3, 6})});
}

TEST_CASE_TEMPLATE("analysis: an already-asserting conflict clause is learned unchanged", Tag,
                   DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    const Conflict conflict = make_two_level_conflict(backend, ClauseId{5});
    RecordingDecisions decisions(backend, conflict.trail.num_vars());
    std::vector<ClauseId> reason_ids;

    const AssertingClause learned =
        find_asserting_clause(conflict.trail, conflict.clauses, decisions, &reason_ids);

    // Clause 5 = [x3 + x4 = 1] v [x5 = 0] already reaches only one decision
    // level beyond its assertion point, so the loop body never runs. Its first
    // row spans two current-level trail rows at once, which is the case that has
    // no literal-level analogue: a single parity "literal" can straddle the whole
    // level and still leave the clause asserting.
    CHECK(rows_of(learned.clause) == rows_of(conflict.clauses[5]));
    CHECK(learned.asserting_level == 1);
    check_asserts_at(learned, conflict.trail);

    CHECK(reason_ids == std::vector<ClauseId>{5});
    CHECK(decisions.events.empty());
}

TEST_CASE_TEMPLATE("analysis: reason ids are collected in derivation order", Tag, DenseTag,
                   SparseTag) {
    constexpr Algebra backend = Tag::value;
    const Conflict conflict = make_two_level_conflict(backend, ClauseId{4});
    RecordingDecisions decisions(backend, conflict.trail.num_vars());

    // The buffer is appended to, never cleared, and the caller reverses it once
    // to obtain proof order: the reversed sequence ends with the conflicting
    // clause and begins with the last reason resolved.
    std::vector<ClauseId> reason_ids{99};
    const AssertingClause learned =
        find_asserting_clause(conflict.trail, conflict.clauses, decisions, &reason_ids);
    CHECK(reason_ids == std::vector<ClauseId>{99, 4, 3, 2});

    // And the buffer really is optional.
    RecordingDecisions other_decisions(backend, conflict.trail.num_vars());
    const AssertingClause without_ids =
        find_asserting_clause(conflict.trail, conflict.clauses, other_decisions, nullptr);
    CHECK(rows_of(without_ids.clause) == rows_of(learned.clause));
    CHECK(without_ids.asserting_level == learned.asserting_level);
    CHECK(other_decisions.events == decisions.events);
}

TEST_CASE_TEMPLATE("analysis: one resolution step in trail-basis coordinates", Tag, DenseTag,
                   SparseTag) {
    constexpr Algebra backend = Tag::value;
    const Conflict conflict = make_two_level_conflict(backend, ClauseId{4});
    const Trail& trail = conflict.trail;
    RecordingDecisions decisions(backend, trail.num_vars());

    // Clause 4's three rows, in trail-basis coordinates. Column c is trail step
    // m - 1 - c with m = 7, so:
    //   [x3 + x4 = 1] = span rows 3 + 4 = trail steps 4, 5 -> columns 2, 1
    //   [x2 = 1]      = span row 2      = trail step 3     -> column 3
    //   [x5 = 1]      = span row 5      = trail step 1     -> column 5
    TrailBasisSpace basis = TrailBasisSpace::from_RowSpace(conflict.clauses[4], trail);
    REQUIRE(rows_of(basis) == std::vector<std::vector<std::size_t>>{{1, 2}, {3}, {5}});

    // The coordinate vector of 1: the unit vector at column 0, because the most
    // recently learned trail row is 1 itself on a conflicting trail.
    const Equation trail_basis_one =
        make_equation_with(backend, basis.num_vars(), {0});
    REQUIRE(trail_basis_one.size() == basis.cols());

    std::vector<ClauseId> reason_ids;
    resolve_asserting_basis_with_reason(basis, 1, trail, conflict.clauses, decisions,
                                        trail_basis_one, &reason_ids);

    // Column 1 is trail step 5, propagated by clause 3 = [x2 = 0] v [x4 = 1].
    // Its own basis rows are {0, 1} (the propagated row [x4 = 0], which carries
    // the 1 coordinate) and {3} (the falsified row [x2 = 1]). So
    //
    //   new row = {1, 2} + {0, 1} + {0} = {2},
    //
    // and the reason's remaining row {3} joins the basis. Without the + 1 term
    // the result would be {0, 2}, a row leading on column 0 -- whose "reason" is
    // the conflicting clause itself, so the next step would resolve clause 4
    // against itself instead of terminating.
    CHECK(rows_of(basis) == std::vector<std::vector<std::size_t>>{{2}, {3}, {5}});
    CHECK(reason_ids == std::vector<ClauseId>{3});
    CHECK(decisions.events ==
          std::vector<RecordingDecisions::Event>{resolved(3), critical({4, 6})});
}

TEST_CASE_TEMPLATE("analysis: the loop runs while the asserting level is the current level", Tag,
                   DenseTag, SparseTag) {
    constexpr Algebra backend = Tag::value;
    const Conflict conflict = make_two_level_conflict(backend, ClauseId{4});
    const Trail& trail = conflict.trail;
    RecordingDecisions decisions(backend, trail.num_vars());

    // Walk the same loop by hand, checking the condition's sign at every step.
    // The loop condition is `asserting_level() == trail.level()`, not `== 0`:
    // here the initial basis has asserting level 2 and the final one 1, so
    // testing against 0 would exit before doing anything, while testing against
    // `trail.level()` keeps going for exactly two steps.
    TrailBasisSpace basis = TrailBasisSpace::from_RowSpace(conflict.clauses[4], trail);
    const Equation trail_basis_one = make_equation_with(backend, basis.num_vars(), {0});

    std::vector<std::size_t> asserting_levels{basis.asserting_level()};
    std::size_t steps = 0;
    while (basis.asserting_level() == trail.level()) {
        const std::optional<std::size_t> pivot_column = basis.row(0).leading_column();
        REQUIRE(pivot_column.has_value());
        resolve_asserting_basis_with_reason(basis, *pivot_column, trail, conflict.clauses,
                                            decisions, trail_basis_one, nullptr);
        // Each step strictly advances the pivot column, which is why the loop
        // terminates at all.
        REQUIRE(basis.row(0).leading_column() > pivot_column);
        asserting_levels.push_back(basis.asserting_level());
        ++steps;
        REQUIRE(steps <= trail.span().rows());
    }

    CHECK(steps == 2);
    CHECK(asserting_levels == std::vector<std::size_t>{2, 2, 1});
}

} // namespace xorcle
