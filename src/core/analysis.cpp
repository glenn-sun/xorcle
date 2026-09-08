#include "core/analysis.h"

#include <algorithm>
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

ResolvedReason resolve_asserting_basis(TrailBasisSpace& asserting_basis, std::size_t pivot_row_idx,
                                       std::size_t pivot_col_idx, const Trail& trail,
                                       const std::vector<Clause>& clauses,
                                       const Equation& trail_basis_one, bool measure_lbd) {
    assert(pivot_row_idx < asserting_basis.rows() && "the pivot row is out of range");
    assert(asserting_basis.row(pivot_row_idx).leading_bit() ==
               std::optional<std::size_t>{pivot_col_idx} &&
           "the pivot column does not lead the pivot row");
    assert(is_trail_basis_one(trail_basis_one, asserting_basis) &&
           "trail_basis_one is not the unit vector at column 0");
    assert(trail_is_conflicting(trail) && "resolution step on a trail that does not contain 1");

    Equation asserting_row = asserting_basis.pop_row(pivot_row_idx);

    const std::size_t trail_index = asserting_basis.cols() - 1 - pivot_col_idx;
    assert(trail_index < trail.reasons().size() && "trail reason index is out of range");
    const std::optional<ClauseId> reason_clause_id = trail.reasons()[trail_index];
    assert(reason_clause_id.has_value() && "resolution step reached a decision, which has no reason");
    assert(*reason_clause_id < clauses.size() && "resolution reason id is out of range");

    TrailBasisSpace reason_basis =
        TrailBasisSpace::from_RowSpace(clauses[*reason_clause_id], trail);
    assert(reason_basis.rows() > 0 && "reason clause has no rows in the trail basis");
    const std::optional<std::size_t> reason_lbd =
        measure_lbd ? std::optional<std::size_t>{reason_basis.literal_block_distance()}
                    : std::nullopt;
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

    // The rows before the pivot are untouched; everything the step leaves or
    // brings in leads strictly later.
    assert((asserting_basis.rows() == pivot_row_idx ||
            *asserting_basis.row(pivot_row_idx).leading_bit() > pivot_col_idx) &&
           "resolution did not eliminate the pivot column");

    return ResolvedReason{*reason_clause_id, reason_lbd};
}

ResolvedReason resolve_asserting_basis_with_reason(TrailBasisSpace& asserting_basis,
                                                   std::size_t pivot_col_idx, const Trail& trail,
                                                   const std::vector<Clause>& clauses,
                                                   DecisionProcedure& decisions,
                                                   const Equation& trail_basis_one,
                                                   bool measure_lbd) {
    const std::size_t trail_index = asserting_basis.cols() - 1 - pivot_col_idx;
    // The 1-UIP loop has passed over no row, so the pivot leads the first one.
    const ResolvedReason reason = resolve_asserting_basis(
        asserting_basis, 0, pivot_col_idx, trail, clauses, trail_basis_one, measure_lbd);

    // Order-sensitive: this and the critical-equation notification below both
    // feed CSIDS, so reordering them changes conflict counts.
    decisions.update_with_resolved_clause(reason.clause_id);

    assert(trail_index < trail.order_map().size() && "trail order index is out of range");
    assert(trail.order_map()[trail_index] < trail.span().rows() &&
           "trail order map points outside the span");
    // The row-echelon row, not the raw equation handed to `Trail::learn()`; the
    // two differ by earlier rows.
    decisions.update_with_critical_equation(trail.span().row(trail.order_map()[trail_index]));

    assert(asserting_basis.rows() > 0 && "resolution derived the empty clause below the root");

    return reason;
}

std::vector<ClauseId> find_root_conflict_reasons(const Trail& trail,
                                                 const std::vector<Clause>& clauses) {
    assert(trail.level() == 0 && "the root conflict chain is only defined at the root");
    assert(trail.span().rows() > 0 && "a conflicting trail cannot be empty");
    assert(trail_is_conflicting(trail) && "the root conflict trail does not contain 1");

    // `reasons()` is in trail order, so its last entry closed the conflict.
    const std::optional<ClauseId> conflict_clause_id = trail.reasons().back();
    assert(conflict_clause_id.has_value() && "the conflicting trail row has no reason");
    assert(*conflict_clause_id < clauses.size() && "conflicting reason id is out of range");

    std::vector<ClauseId> reason_ids{*conflict_clause_id};
    TrailBasisSpace basis = TrailBasisSpace::from_RowSpace(clauses[*conflict_clause_id], trail);
    const Equation trail_basis_one = make_trail_basis_one(trail);

    // At the root every row has a reason, so the walk resolves the basis away.
    while (basis.rows() > 0) {
        const std::optional<std::size_t> pivot_col_idx = basis.row(0).leading_bit();
        assert(pivot_col_idx.has_value() && "a stored row is zero");
        assert(reason_ids.size() <= basis.cols() && "the root conflict walk did not terminate");
        const ResolvedReason reason =
            resolve_asserting_basis(basis, 0, *pivot_col_idx, trail, clauses, trail_basis_one);
        reason_ids.push_back(reason.clause_id);
    }

    // Derivation order to replay order. Exactly one reversal, here.
    std::reverse(reason_ids.begin(), reason_ids.end());
    return reason_ids;
}

AssertingClause find_asserting_clause(const Trail& trail, const std::vector<Clause>& clauses,
                                      DecisionProcedure& decisions, bool minimize,
                                      bool measure_reason_lbds) {
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

    std::vector<std::size_t> reason_lbds;
    if (measure_reason_lbds) {
        reason_lbds.push_back(asserting_basis.literal_block_distance());
    }

    const Equation trail_basis_one = make_trail_basis_one(trail);

    // `frozen` counts the leading rows the walk has passed over. They can stay
    // in the matrix: they lead strictly earlier than the pivot, while every row
    // a resolution leaves or brings in leads strictly later.
    std::size_t frozen = 0;
    std::size_t resolution_steps = 0;
    std::size_t width_before_minimize = 0;
    for (bool asserting = false; frozen < asserting_basis.rows();) {
        const std::optional<std::size_t> pivot_col_idx = asserting_basis.row(frozen).leading_bit();
        assert(pivot_col_idx.has_value() && "a stored row is zero");

        if (!asserting) {
            if (asserting_basis.asserting_level() < trail.level()) {
                asserting = true;
                width_before_minimize = asserting_basis.rows();
                if (!minimize) {
                    break;
                }
                continue;
            }
            ++resolution_steps;
            assert(resolution_steps < asserting_basis.cols() &&
                   "conflict analysis did not terminate");
            const ResolvedReason reason = resolve_asserting_basis_with_reason(
                asserting_basis, *pivot_col_idx, trail, clauses, decisions, trail_basis_one,
                measure_reason_lbds);
            reason_ids.push_back(reason.clause_id);
            if (measure_reason_lbds) {
                reason_lbds.push_back(*reason.lbd);
            }
            continue;
        }

        // Minimization: continue the walk to the start of the trail, accepting a
        // resolution only when it strictly shrinks the clause; decision-led rows
        // are kept. Only an accepted step reaches the heuristics and the proof.
        const std::size_t trail_index = asserting_basis.cols() - 1 - *pivot_col_idx;
        assert(trail_index < trail.reasons().size() && "trail reason index is out of range");
        if (trail.reasons()[trail_index].has_value()) {
            TrailBasisSpace resolved = asserting_basis;
            const ResolvedReason reason = resolve_asserting_basis(
                resolved, frozen, *pivot_col_idx, trail, clauses, trail_basis_one,
                measure_reason_lbds);
            // A resolvent with no rows would be the empty clause below the root,
            // which a well-formed run cannot reach.
            if (resolved.rows() < asserting_basis.rows() && resolved.rows() > 0) {
                asserting_basis = std::move(resolved);
                reason_ids.push_back(reason.clause_id);
                if (measure_reason_lbds) {
                    reason_lbds.push_back(*reason.lbd);
                }
                decisions.update_with_resolved_clause(reason.clause_id);
                decisions.update_with_critical_equation(
                    trail.span().row(trail.order_map()[trail_index]));
                continue;
            }
        }
        ++frozen;
    }

    assert(width_before_minimize > 0 && "the walk never reached the asserting point");
    assert(asserting_basis.rows() <= width_before_minimize && "minimization widened the clause");

    const std::size_t asserting_level = asserting_basis.asserting_level();
    assert(asserting_level < trail.level() && "the walk exited without discarding a level");

    // Only the trail-basis coordinates carry it; the clause itself does not.
    const std::size_t lbd = asserting_basis.literal_block_distance();

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

    assert((reason_lbds.empty() || reason_lbds.size() == reason_ids.size()) &&
           "a measured reason is missing its literal block distance");

    return AssertingClause{std::move(asserting_clause),
                          asserting_level,
                          std::move(reason_ids),
                          std::move(reason_lbds),
                          lbd,
                          width_before_minimize};
}

} // namespace xorcle
