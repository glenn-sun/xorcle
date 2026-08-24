#include "core/analysis.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/reduce_result.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "core/trail_basis.h"

namespace xorcle {

namespace {

/// @brief The unit vector at column 0.
Equation make_trail_basis_one(const Trail& trail) {
    const std::size_t num_coefficients = trail.span().rows();
    assert(num_coefficients > 0 && "conflict analysis on an empty trail span");
    Equation one = Equation::with_num_bits(trail.repr(), num_coefficients);
    const std::array<std::size_t, 1> bits{0};
    one.assign_bits(std::span<const std::size_t>(bits));
    return one;
}

#ifndef NDEBUG
bool trail_is_conflicting(const Trail& trail) {
    Equation one = Equation::eq_false(trail.repr(), trail.num_vars());
    return trail.span().partial_reduce(one).kind == ReduceResult::Kind::eq_true;
}

bool is_trail_basis_one(const Equation& eqn, const TrailBasisSpace& basis) {
    return eqn.num_bits() == basis.cols() && eqn.bits() == std::vector<std::size_t>{0};
}
#endif

} // namespace

ClauseId resolve_asserting_basis_with_reason(TrailBasisSpace& asserting_basis,
                                             std::size_t pivot_column, const Trail& trail,
                                             const std::vector<Clause>& clauses,
                                             DecisionProcedure& decisions,
                                             const Equation& trail_basis_one) {
    assert(asserting_basis.rows() > 0 && "resolving against an empty basis");
    assert(asserting_basis.row(0).leading_bit() == std::optional<std::size_t>{pivot_column} &&
           "the pivot column is not the leading column of the first row");
    assert(is_trail_basis_one(trail_basis_one, asserting_basis) &&
           "trail_basis_one is not the unit vector at column 0");
    assert(trail_is_conflicting(trail) && "resolution step on a trail that does not contain 1");

    Equation asserting_row = asserting_basis.pop_first_row();

    const std::size_t reason_index = asserting_basis.cols() - 1 - pivot_column;
    assert(reason_index < trail.reasons().size() && "trail reason index is out of range");
    const std::optional<ClauseId> reason_clause_id = trail.reasons()[reason_index];
    assert(reason_clause_id.has_value() && "resolution step reached a decision, which has no reason");
    assert(*reason_clause_id < clauses.size() && "resolution reason id is out of range");

    const Clause& reason_clause = clauses[*reason_clause_id];

    // Order-sensitive: this and the critical-equation notification at the end of
    // the function both feed CSIDS, so reordering them changes conflict counts.
    decisions.update_with_resolved_clause(*reason_clause_id);

    TrailBasisSpace reason_basis = TrailBasisSpace::from_RowSpace(reason_clause, trail);
    assert(reason_basis.rows() > 0 && "reason clause has no rows in the trail basis");
    Equation reason_row = reason_basis.pop_first_row();

    // The explicit `+ [0 = 1]` is the linear negation, valid because on a
    // conflicting trail column 0 is provably `0 = 1`. Without it the loop does
    // not terminate.
    asserting_row.plus_assign(reason_row);
    asserting_row.plus_assign(trail_basis_one);
    asserting_basis.add_row(asserting_row);
    for (const Equation& extra_reason_row : reason_basis) {
        asserting_basis.add_row(extra_reason_row);
    }

    assert(reason_index < trail.order_map().size() && "trail order index is out of range");
    assert(trail.order_map()[reason_index] < trail.span().rows() &&
           "trail order map points outside the span");
    // The row-echelon row, not the raw equation handed to `Trail::learn()`; the
    // two differ by earlier rows.
    decisions.update_with_critical_equation(trail.span().row(trail.order_map()[reason_index]));

    assert(asserting_basis.rows() > 0 && "resolution derived the empty clause below the root");
    assert(*asserting_basis.row(0).leading_bit() > pivot_column &&
           "resolution did not eliminate the pivot column");

    return *reason_clause_id;
}

AssertingClause find_asserting_clause(const Trail& trail, const std::vector<Clause>& clauses,
                                      DecisionProcedure& decisions) {
    assert(trail.span().rows() > 0 && "a conflicting trail cannot be empty");
    assert(trail.level() > 0 && "a root conflict is UNSAT and is not analysed");
    assert(trail_is_conflicting(trail) && "conflict analysis on a trail that does not contain 1");

    // `reasons()` is in trail order, so its last entry closed the conflict.
    const std::optional<ClauseId> conflict_clause_id = trail.reasons()[trail.span().rows() - 1];
    assert(conflict_clause_id.has_value() && "the conflicting trail row has no reason");
    assert(*conflict_clause_id < clauses.size() && "conflicting reason id is out of range");

    std::vector<ClauseId> reason_ids{*conflict_clause_id};

    TrailBasisSpace asserting_basis =
        TrailBasisSpace::from_RowSpace(clauses[*conflict_clause_id], trail);

    const Equation trail_basis_one = make_trail_basis_one(trail);

    std::size_t resolution_steps = 0;
    while (asserting_basis.asserting_level() == trail.level()) {
        ++resolution_steps;
        assert(resolution_steps < asserting_basis.cols() && "conflict analysis did not terminate");
        assert(asserting_basis.rows() > 0 && "resolution emptied the asserting basis");
        const std::optional<std::size_t> pivot_column = asserting_basis.row(0).leading_bit();
        assert(pivot_column.has_value() && "a stored row is zero");
        reason_ids.push_back(resolve_asserting_basis_with_reason(
            asserting_basis, *pivot_column, trail, clauses, decisions, trail_basis_one));
    }

    const std::size_t asserting_level = asserting_basis.asserting_level();
    assert(asserting_level < trail.level() && "the loop exited without discarding a level");

    // Copied row by row so the clause gets the watches of a freshly built clause.
    const RowSpace asserting_rows = asserting_basis.to_RowSpace();
    Clause asserting_clause(trail.repr(), trail.num_vars());
    for (const Equation& row : asserting_rows) {
        asserting_clause.add_row(row);
    }
    assert(asserting_clause.rows() == asserting_basis.rows() &&
           "the asserting clause lost a row in conversion");
    assert(asserting_clause.cols() == trail.span().cols() &&
           "the asserting clause is over the wrong variable count");

    assert((asserting_clause.rows() != 1 || asserting_level == 0) &&
           "a unit asserting clause must assert at the root");

    return AssertingClause{std::move(asserting_clause), asserting_level, std::move(reason_ids)};
}

} // namespace xorcle
