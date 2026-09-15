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

/// @brief Whether `replacement` spans exactly what `basis` converts back to.
bool replacement_matches_resolvent(const Clause& replacement, const TrailBasisSpace& basis,
                                   const Trail& trail) {
    if (replacement.rows() != basis.rows()) {
        return false;
    }
    const RowSpace resolvent = basis.to_RowSpace();
    Equation scratch(trail.repr(), trail.num_vars());
    for (const Equation& row : resolvent) {
        scratch.assign_from(row);
        if (replacement.partial_reduce(scratch).kind != ReduceResult::Kind::eq_true) {
            return false;
        }
    }
    return true;
}

bool is_trail_basis_one(const Equation& eqn, const TrailBasisSpace& basis) {
    return eqn.num_bits() == basis.cols() && eqn.bits() == std::vector<std::size_t>{0};
}
#endif

} // namespace

ResolvedReason resolve_asserting_basis(
    TrailBasisSpace& asserting_basis, std::size_t pivot_row_idx, std::size_t pivot_col_idx,
    const Trail& trail, const std::vector<Clause>& clauses, const Equation& trail_basis_one,
    bool measure_lbd, std::vector<std::size_t>* reason_row_leading_cols) {
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
        TrailBasisSpace::from_RowSpace(clauses[*reason_clause_id], trail, reason_row_leading_cols);
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

    // The resolvent always spans the reason's remaining rows, so rank equal to
    // their count means equality of spans: the resolvent subsumes the reason.
    return ResolvedReason{*reason_clause_id, reason_lbd,
                          asserting_basis.rows() == reason_basis.rows()};
}

ResolvedReason resolve_asserting_basis_with_reason(
    TrailBasisSpace& asserting_basis, std::size_t pivot_col_idx, const Trail& trail,
    const std::vector<Clause>& clauses, DecisionProcedure& decisions,
    const Equation& trail_basis_one, bool measure_lbd,
    std::vector<std::size_t>* reason_row_leading_cols) {
    const std::size_t trail_index = asserting_basis.cols() - 1 - pivot_col_idx;
    // The 1-UIP loop has passed over no row, so the pivot leads the first one.
    const ResolvedReason reason =
        resolve_asserting_basis(asserting_basis, 0, pivot_col_idx, trail, clauses,
                                trail_basis_one, measure_lbd, reason_row_leading_cols);

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
                                      bool measure_reason_lbds, bool report_pivot_trail_idxs,
                                      bool collect_subsumed) {
    assert(trail.span().rows() > 0 && "a conflicting trail cannot be empty");
    assert(trail.level() > 0 && "a root conflict is UNSAT and is not analysed");
    assert(trail_is_conflicting(trail) && "conflict analysis on a trail that does not contain 1");

    // `reasons()` is in trail order, so its last entry closed the conflict.
    const std::optional<ClauseId> conflict_clause_id = trail.reasons()[trail.span().rows() - 1];
    assert(conflict_clause_id.has_value() && "the conflicting trail row has no reason");
    assert(*conflict_clause_id < clauses.size() && "conflicting reason id is out of range");

    std::vector<ClauseId> reason_ids{*conflict_clause_id};

    std::vector<std::size_t> conflict_row_leading_cols;
    TrailBasisSpace asserting_basis = TrailBasisSpace::from_RowSpace(
        clauses[*conflict_clause_id], trail,
        collect_subsumed ? &conflict_row_leading_cols : nullptr);
    const std::size_t conflict_clause_rank = asserting_basis.rows();

    std::vector<std::size_t> reason_lbds;
    if (measure_reason_lbds) {
        reason_lbds.push_back(asserting_basis.literal_block_distance());
    }

    const Equation trail_basis_one = make_trail_basis_one(trail);

    // Copied row by row so the clause gets the watches of a freshly built clause.
    const auto to_clause = [&trail](const TrailBasisSpace& basis) {
        const RowSpace rows = basis.to_RowSpace();
        Clause clause(trail.repr(), trail.num_vars());
        for (const Equation& row : rows) {
            clause.add_row(row);
        }
        return clause;
    };

    std::vector<SubsumedReason> subsumed_reasons;
    std::vector<std::size_t> reason_row_leading_cols;
    std::vector<std::size_t>* const leading_cols_out =
        collect_subsumed ? &reason_row_leading_cols : nullptr;
    Equation combined_row(trail.repr(), trail.num_vars());
    // The replacement is the reason clause with its most recent coordinate
    // cancelled: a subsumption drops the reason's *first* echelon row, which leads at
    // the minimum leading column of its rows (the conflict-row direction when
    // the coordinates touch it, the pivot column otherwise). Rows leading
    // elsewhere stay, and each further row leading there absorbs the first
    // such row. Same span as the resolvent, no trail-length factor.
    const auto record_subsumed = [&](const TrailBasisSpace& basis, ClauseId reason_id,
                                     std::size_t trail_idx, std::size_t num_reasons,
                                     const std::vector<std::size_t>& row_leading_cols,
                                     bool conflicting) {
        const Clause& reason = clauses[reason_id];
        assert(row_leading_cols.size() == reason.rows() &&
               "the reason's leading columns do not match its rows");
        const std::size_t drop_col = *std::min_element(row_leading_cols.begin(),
                                                       row_leading_cols.end());
        Clause replacement(trail.repr(), trail.num_vars());
        std::optional<std::size_t> first_drop_row;
        for (std::size_t i = 0; i < reason.rows(); ++i) {
            if (row_leading_cols[i] != drop_col) {
                replacement.add_row(reason.row(i));
            } else if (!first_drop_row.has_value()) {
                first_drop_row = i;
            } else {
                combined_row.assign_from(reason.row(i));
                combined_row.plus_assign(reason.row(*first_drop_row));
                replacement.add_row(combined_row);
            }
        }
        assert(replacement_matches_resolvent(replacement, basis, trail) &&
               "the rebuilt replacement does not span the resolvent");
        subsumed_reasons.push_back(SubsumedReason{reason_id, std::move(replacement),
                                                  basis.literal_block_distance(),
                                                  num_reasons, trail_idx, conflicting});
    };

    // The first resolution is the one step whose left operand is also a stored
    // clause: the conflicting clause. Its rank dropping to one less means the
    // resolvent subsumes it too, exactly as for the reason side.
    bool conflicting_clause_check_open = true;
    const auto maybe_record_subsumed_conflicting = [&](std::size_t pivot_row_idx) {
        if (conflicting_clause_check_open && pivot_row_idx == 0 &&
            asserting_basis.rows() + 1 == conflict_clause_rank) {
            record_subsumed(asserting_basis, *conflict_clause_id, trail.span().rows() - 1,
                            reason_ids.size(), conflict_row_leading_cols, true);
        }
        conflicting_clause_check_open = false;
    };

    // Set while the latest accepted resolution subsumed its reason: then the
    // clause as learned is that record's replacement, and no copy is added.
    std::optional<std::size_t> last_resolution_record_idx;

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
                measure_reason_lbds, leading_cols_out);
            reason_ids.push_back(reason.clause_id);
            if (measure_reason_lbds) {
                reason_lbds.push_back(*reason.lbd);
            }
            if (collect_subsumed) {
                maybe_record_subsumed_conflicting(0);
                if (reason.subsumes_reason) {
                    record_subsumed(asserting_basis, reason.clause_id,
                                    asserting_basis.cols() - 1 - *pivot_col_idx,
                                    reason_ids.size(), reason_row_leading_cols, false);
                    last_resolution_record_idx = subsumed_reasons.size() - 1;
                } else {
                    last_resolution_record_idx.reset();
                }
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
                measure_reason_lbds, leading_cols_out);
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
                if (collect_subsumed) {
                    maybe_record_subsumed_conflicting(frozen);
                    if (reason.subsumes_reason) {
                        record_subsumed(asserting_basis, reason.clause_id, trail_index,
                                        reason_ids.size(), reason_row_leading_cols, false);
                        last_resolution_record_idx = subsumed_reasons.size() - 1;
                    } else {
                        last_resolution_record_idx.reset();
                    }
                }
                continue;
            }
            // The discarded resolvent still subsumes its reason, so harvest the
            // replacement anyway; the walk keeps the wider clause. Its chain is
            // one longer than `reason_ids`, which never received this reason.
            if (collect_subsumed && reason.subsumes_reason) {
                record_subsumed(resolved, reason.clause_id, trail_index,
                                reason_ids.size() + 1, reason_row_leading_cols, false);
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

    std::vector<std::size_t> pivot_trail_idxs;
    if (report_pivot_trail_idxs) {
        pivot_trail_idxs.reserve(asserting_basis.rows());
        for (const Equation& row : asserting_basis) {
            assert(row.leading_bit().has_value() && "a stored row is zero");
            pivot_trail_idxs.push_back(asserting_basis.cols() - 1 - *row.leading_bit());
        }
    }

    // The clause as learned is that record's replacement; the caller learns it
    // rather than adding a copy.
    if (last_resolution_record_idx.has_value()) {
        subsumed_reasons[*last_resolution_record_idx].replacement.reset();
    }

    Clause asserting_clause = to_clause(asserting_basis);
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
                          width_before_minimize,
                          std::move(pivot_trail_idxs),
                          std::move(subsumed_reasons)};
}

} // namespace xorcle
