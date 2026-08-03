#include "core/analysis.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "core/trail_basis.h"

namespace xorcle {

namespace {

/// @brief The coordinate vector of @f$\mathbf{1}@f$ in the trail basis.
///
/// Column 0 of the trail basis is the coordinate of the most recently learned
/// trail row, and on a conflicting trail that row *is* @f$\mathbf{1}@f$ -- see
/// the derivation in `resolve_asserting_basis_with_reason()`. So the coordinate
/// vector wanted is exactly the unit vector at column 0.
/// @param trail The conflicting trail, supplying the backend and the width.
Equation make_trail_basis_one(const Trail& trail) {
    const std::size_t coefficient_width = trail.span().rows();
    assert(coefficient_width > 0 && "conflict analysis on an empty trail span");
    const std::array<std::size_t, 1> columns{0};
    return Equation::from_columns(trail.backend(), coefficient_width - 1,
                                  std::span<const std::size_t>(columns));
}

#ifndef NDEBUG
/// @brief Whether `eqn` is the trail-basis coordinate vector of @f$\mathbf{1}@f$.
bool is_trail_basis_one(const Equation& eqn, const TrailBasisSpace& basis) {
    std::vector<std::size_t> set_columns;
    eqn.columns(set_columns);
    return eqn.size() == basis.cols() && set_columns == std::vector<std::size_t>{0};
}
#endif

} // namespace

void resolve_asserting_basis_with_reason(TrailBasisSpace& asserting_basis,
                                         std::size_t pivot_column, const Trail& trail,
                                         const std::vector<Clause>& clauses,
                                         DecisionProcedure& decisions,
                                         const Equation& trail_basis_one,
                                         std::vector<ClauseId>* reason_ids) {
    assert(asserting_basis.rows() > 0 && "resolving against an empty basis");
    assert(asserting_basis.row(0).leading_column() == std::optional<std::size_t>{pivot_column} &&
           "the pivot column is not the leading column of the first row");
    assert(is_trail_basis_one(trail_basis_one, asserting_basis));

    // Row 0 has the smallest leading column, so it is the row that reaches
    // furthest forward in trail order: exactly the one to resolve away.
    Equation asserting_row = asserting_basis.pop_first_row();

    // Trail-basis column `c` is trail step `m - 1 - c`, so the coordinate being
    // eliminated names the trail step whose reason clause explains it.
    const std::size_t reason_index = asserting_basis.cols() - 1 - pivot_column;
    assert(reason_index < trail.reasons().size() && "trail reason index is out of range");
    const std::optional<ClauseId> reason_clause_id = trail.reasons()[reason_index];
    assert(reason_clause_id.has_value() && "resolution step reached a decision, which has no reason");
    assert(*reason_clause_id < clauses.size() && "resolution reason id is out of range");
    if (reason_ids != nullptr) {
        reason_ids->push_back(*reason_clause_id);
    }

    // Read the reason clause by reference. The 0.1 tree wrote
    // `algebra::RowSpace reason_clause = get_clause(id);`, which both sliced the
    // `Clause` down to a `RowSpace` and deep-copied the whole matrix, on the
    // conflict-analysis hot path.
    const Clause& reason_clause = clauses[*reason_clause_id];

    // Order-sensitive: the resolved-clause bump fires *before* the basis is
    // built, and the critical-equation notification fires last, at the bottom of
    // this function. Both feed CSIDS, so reordering them changes conflict counts
    // without changing correctness -- which the differential harness sees and
    // nothing else does.
    decisions.update_with_resolved_clause(*reason_clause_id);

    // Every row of the reason clause lies in the trail span, which is what
    // `from_RowSpace()` requires. That is not an accident of this call site, it
    // is the same conflicting-trail fact the resolution identity rests on: the
    // rows the reason falsified are in the span by definition, and the row it
    // propagated satisfies `g + 1 = e`, with both `e` and `1` in the span.
    TrailBasisSpace reason_basis = TrailBasisSpace::from_RowSpace(reason_clause, trail);
    assert(reason_basis.rows() > 0 && "reason clause has no rows in the trail basis");
    Equation reason_row = reason_basis.pop_first_row();

    // -------------------------------------------------------------------------
    // The resolution step, and why it carries an extra 1.
    //
    // Trail-basis column 0 is the coordinate of the most recently learned trail
    // row. At the moment conflict analysis runs, that row is provably the
    // constant equation 1 = [0 ... 0 1]:
    //
    //   the solver learns each propagated equation and only then tests for a
    //   conflict, by reducing 1 against the span; the test succeeds exactly when
    //   the constant column has become a pivot and its pivot row is literally
    //   [0 ... 0 1]. The constant column was not a pivot before the last
    //   `learn()`, so the row that last `learn()` created *is* that pivot row.
    //   Having the largest possible leading column it is the last row in
    //   row-echelon order, and being the last thing learned its trail step is
    //   m - 1, i.e. trail-basis column 0.
    //
    // So `trail_basis_one` -- the unit vector at column 0 -- is the coordinate
    // vector of 1, and the step below is
    //
    //     new row = asserting row + reason row + 1.
    //
    // That is ordinary 1-UIP resolution written in the linear negation. The
    // reason clause's propagated row is g = e + 1 for the trail equation e it
    // implied, so in trail coordinates it contributes e and an extra 1; adding 1
    // back restores the negation the trail basis absorbed. Concretely the term
    // is what cancels column 0, which `reason_row` always carries and
    // `asserting_row` never does, and column `pivot_column`, which both carry.
    // Dropping it leaves a row leading on column 0, whose "reason" is the
    // conflicting clause itself, and the loop resolves against that clause again
    // instead of terminating.
    //
    // **This is valid only on a conflicting trail.** Both the identity above and
    // the `from_RowSpace()` precondition come from 1 being in the span, so
    // calling this outside conflict analysis, or reordering the learn-then-check
    // sequence in the solver's propagation loop, breaks it silently.
    // -------------------------------------------------------------------------
    asserting_row.xor_assign(reason_row);
    asserting_row.xor_assign(trail_basis_one);
    asserting_basis.add_row(asserting_row);
    for (const Equation& extra_reason_row : reason_basis) {
        asserting_basis.add_row(extra_reason_row);
    }

    assert(reason_index < trail.order_map().size());
    assert(trail.order_map()[reason_index] < trail.span().rows());
    // Fires last, and on the row-echelon row rather than the raw trail equation
    // the solver handed to `Trail::learn()`. The two differ by a combination of
    // earlier rows.
    decisions.update_with_critical_equation(trail.span().row(trail.order_map()[reason_index]));

    // Both eliminated coordinates are gone and nothing the reason contributed
    // reaches back into them, so the next step resolves a strictly later column
    // and the loop terminates. The 0.1 tree asserted this as
    // `for (c = 0; c <= pivot_column; ++c) assert(basis.col(c).none());`, which
    // over rows in ascending leading-column order is this one comparison.
    assert(asserting_basis.rows() > 0 && "resolution derived the empty clause below the root");
    assert(*asserting_basis.row(0).leading_column() > pivot_column &&
           "resolution did not eliminate the pivot column");
}

AssertingClause find_asserting_clause(const Trail& trail, const std::vector<Clause>& clauses,
                                      DecisionProcedure& decisions,
                                      std::vector<ClauseId>* reason_ids) {
    assert(trail.span().rows() > 0 && "a conflicting trail cannot be empty");
    assert(trail.level() > 0 && "a root conflict is UNSAT and is not analysed");

    // `reasons()` is indexed by trail order and has one entry per span row, so
    // the last entry is the equation that closed the conflict and its reason is
    // the conflicting clause.
    const std::optional<ClauseId> conflict_clause_id = trail.reasons()[trail.span().rows() - 1];
    assert(conflict_clause_id.has_value() && "the conflicting trail row has no reason");
    assert(*conflict_clause_id < clauses.size() && "conflicting reason id is out of range");
    if (reason_ids != nullptr) {
        // First id appended, so the caller's reversed view ends with it.
        reason_ids->push_back(*conflict_clause_id);
    }

    TrailBasisSpace asserting_basis =
        TrailBasisSpace::from_RowSpace(clauses[*conflict_clause_id], trail);

    // Preallocated once per conflict rather than once per resolution step. Its
    // width is the trail span's row count, which cannot change here: conflict
    // analysis reads the trail and never learns into it.
    const Equation trail_basis_one = make_trail_basis_one(trail);

    // The loop condition, and the one rewrite in this port most likely to be
    // inverted. The 0.1 tree wrote `while (basis.jumpable_levels() == 0)`, i.e.
    // "while no level can be discarded". `asserting_level()` counts from the
    // other end, so "no level can be discarded" is
    // `asserting_level() == trail.level()`, **not** `== 0` -- which is the
    // opposite condition and would stop immediately on almost every conflict.
    //
    // The step counter is a tripwire for exactly that mistake. Every step
    // strictly advances the pivot column, so the loop cannot run more times than
    // the basis has columns; if it does, the condition is inverted or the
    // resolution step stopped eliminating its pivot. Without the check that
    // failure mode is an unbounded loop that adds rows on every pass, which
    // exhausts memory rather than hanging visibly.
    std::size_t resolution_steps = 0;
    while (asserting_basis.asserting_level() == trail.level()) {
        ++resolution_steps;
        assert(resolution_steps < asserting_basis.cols() && "conflict analysis did not terminate");
        assert(asserting_basis.rows() > 0);
        const std::optional<std::size_t> pivot_column = asserting_basis.row(0).leading_column();
        assert(pivot_column.has_value() && "a stored row is zero");
        resolve_asserting_basis_with_reason(asserting_basis, *pivot_column, trail, clauses,
                                            decisions, trail_basis_one, reason_ids);
    }

    const std::size_t asserting_level = asserting_basis.asserting_level();
    assert(asserting_level < trail.level() && "the loop exited without discarding a level");

    // `to_RowSpace()` hands back a `RowSpace`, and `Clause` adds watch state on
    // top of one. The 0.1 tree relied on a converting constructor that sliced;
    // here the rows are copied across explicitly, which also gives the clause the
    // default watches a freshly built clause is supposed to have. The rows
    // arrive already in row-echelon order, so each `add_row()` finds its leading
    // column unpivoted and inserts without reducing.
    const RowSpace lifted = asserting_basis.to_RowSpace();
    Clause asserting_clause(trail.backend(), trail.num_vars());
    for (const Equation& row : lifted) {
        asserting_clause.add_row(row);
    }
    assert(asserting_clause.rows() == asserting_basis.rows());
    assert(asserting_clause.cols() == trail.span().cols());
    // A one-row asserting clause is a unit clause, which asserts at the root.
    assert((asserting_clause.rows() != 1 || asserting_level == 0) &&
           "a unit asserting clause must assert at the root");

    return AssertingClause{std::move(asserting_clause), asserting_level};
}

} // namespace xorcle
