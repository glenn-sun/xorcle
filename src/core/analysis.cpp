#include "core/analysis.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
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
#endif

/// @brief Whether resolving `basis` on the row at `pivot_row_idx` with the
/// reason of the trail row at `reason_trail_idx` leaves a narrower clause.
///
/// The rows before the pivot keep their leading columns and everything the step
/// brings in leads later, so the resolvent is narrower exactly when all it brings
/// in already lies in `basis`, and then it is narrower by the pivot row alone.
bool resolution_narrows(const TrailBasisSpace& basis, std::size_t pivot_row_idx,
                        const TrailCoordinates& reason, std::size_t reason_trail_idx,
                        ResolutionWorkspace& workspace) {
    // The step brings in the reason's rows without `0 = 1`, and the pivot row plus
    // `0 = 1` plus one reason row with it. Once the others lie in `basis`, any
    // reason row with `0 = 1` stands for the one echelonization would pick.
    Equation brought_in = workspace.spare_row();
    Equation with_one;
    bool narrows = true;
    for (std::size_t reason_row_idx = 0; narrows && reason_row_idx < reason.rows();
         ++reason_row_idx) {
        basis.assign_coordinates_row(brought_in, reason, reason_row_idx, reason_trail_idx);
        if (brought_in.leading_bit() == std::optional<std::size_t>{0}) {
            if (!with_one.is_null()) {
                brought_in.plus_assign(with_one);
            } else {
                with_one = workspace.spare_row();
                with_one.assign_from(brought_in);
                brought_in.plus_assign(basis.row(pivot_row_idx));
                brought_in.plus_assign(workspace.trail_basis_one);
            }
        }
        basis.reduce(brought_in);
        narrows = !brought_in.leading_bit().has_value();
    }
    assert((!narrows || !with_one.is_null()) &&
           "the reason does not reach the conflict's 0 = 1");
    workspace.spare_rows.push_back(std::move(brought_in));
    if (!with_one.is_null()) {
        workspace.spare_rows.push_back(std::move(with_one));
    }
    return narrows;
}

} // namespace

ResolutionWorkspace::ResolutionWorkspace(const Trail& trail)
    : trail_basis_one(make_trail_basis_one(trail)), reason_basis(trail) {}

Equation ResolutionWorkspace::spare_row() {
    if (spare_rows.empty()) {
        return Equation::with_num_bits(reason_basis.repr(), reason_basis.cols());
    }
    Equation row = std::move(spare_rows.back());
    spare_rows.pop_back();
    return row;
}

ResolvedReason resolve_asserting_basis(
    TrailBasisSpace& asserting_basis, std::size_t pivot_row_idx, std::size_t pivot_col_idx,
    const Trail& trail, const std::vector<Clause>& clauses, ResolutionWorkspace& workspace,
    Rng& representative_rng, bool measure_lbd) {
    assert(pivot_row_idx < asserting_basis.rows() && "the pivot row is out of range");
    assert(asserting_basis.row(pivot_row_idx).leading_bit() ==
               std::optional<std::size_t>{pivot_col_idx} &&
           "the pivot column does not lead the pivot row");
    assert(workspace.reason_basis.cols() == asserting_basis.cols() &&
           workspace.reason_basis.rows() == 0 &&
           "the workspace was built on another trail, or a step left rows in it");
    assert(trail_is_conflicting(trail) && "resolution step on a trail that does not contain 0 = 1");

    Equation asserting_row = asserting_basis.pop_row(pivot_row_idx);

    // Every earlier row may gain the pivot row and still span a valid resolvent;
    // a fair coin each draws the representative uniformly from that family.
    std::uint64_t coins = 0;
    for (std::size_t row_idx = 0; row_idx < pivot_row_idx; ++row_idx) {
        const std::size_t bit = row_idx % 64;
        if (bit == 0) {
            coins = representative_rng();
        }
        if ((coins >> bit) & 1ULL) {
            asserting_basis.add_to_row(row_idx, asserting_row);
        }
    }

    const std::size_t trail_idx = asserting_basis.cols() - 1 - pivot_col_idx;
    assert(trail_idx < trail.reasons().size() && "trail reason index is out of range");
    const std::optional<ClauseId> reason_clause_id = trail.reasons()[trail_idx];
    assert(reason_clause_id.has_value() && "resolution step reached a decision, which has no reason");
    assert(*reason_clause_id < clauses.size() && "resolution reason id is out of range");

    TrailBasisSpace& reason_basis = workspace.reason_basis;
    reason_basis.add_reason_rows(clauses[*reason_clause_id], trail_idx, workspace.spare_rows);
    assert(reason_basis.rows() > 0 && "reason clause has no rows in the trail basis");
    Equation reason_row = reason_basis.pop_first_row();
    // The popped row leads at column 0, in the conflict's level; the equation
    // the reason propagated counts at its own level instead.
    const std::optional<std::size_t> reason_lbd =
        measure_lbd
            ? std::optional<std::size_t>{reason_basis.literal_block_distance_with(pivot_col_idx)}
            : std::nullopt;

    // The explicit `+ [0 = 1]` is the linear negation, valid because on a
    // conflicting trail column 0 is provably `0 = 1`. Without it the loop does
    // not terminate.
    asserting_row.plus_assign(reason_row);
    asserting_row.plus_assign(workspace.trail_basis_one);
    workspace.spare_rows.push_back(std::move(reason_row));
    if (!asserting_basis.add_row_or_keep(asserting_row).has_value()) {
        workspace.spare_rows.push_back(std::move(asserting_row));
    }
    const std::size_t num_extra_reason_rows = reason_basis.rows();
    while (reason_basis.rows() != 0) {
        Equation extra_row = reason_basis.pop_first_row();
        if (!asserting_basis.add_row_or_keep(extra_row).has_value()) {
            workspace.spare_rows.push_back(std::move(extra_row));
        }
    }

    // The rows before the pivot keep their leading columns; everything the step
    // leaves or brings in leads strictly later.
    assert((asserting_basis.rows() == pivot_row_idx ||
            *asserting_basis.row(pivot_row_idx).leading_bit() > pivot_col_idx) &&
           "resolution did not eliminate the pivot column");

    // The resolvent always spans the reason's remaining rows, so rank equal to
    // their count means equality of spans: the resolvent subsumes the reason.
    return ResolvedReason{*reason_clause_id, reason_lbd,
                          asserting_basis.rows() == num_extra_reason_rows};
}

std::vector<ClauseId> find_root_conflict_reasons(const Trail& trail,
                                                 const std::vector<Clause>& clauses,
                                                 ClauseId conflict_clause_id,
                                                 Rng& representative_rng) {
    assert(trail.level() == 0 && "the root conflict chain is only defined at the root");
    assert(trail.span().rows() > 0 && "a conflicting trail cannot be empty");
    assert(trail_is_conflicting(trail) && "the root conflict trail does not contain 0 = 1");
    assert(conflict_clause_id < clauses.size() && "conflicting reason id is out of range");
    assert(trail.reasons().back() == std::optional<ClauseId>{conflict_clause_id} &&
           "the conflicting clause did not propagate the last trail row");

    std::vector<ClauseId> reason_ids{conflict_clause_id};
    TrailBasisSpace basis = TrailBasisSpace::from_RowSpace(clauses[conflict_clause_id], trail);
    ResolutionWorkspace workspace(trail);

    // At the root every row has a reason, so the walk resolves the basis away.
    while (basis.rows() > 0) {
        const std::optional<std::size_t> pivot_col_idx = basis.row(0).leading_bit();
        assert(pivot_col_idx.has_value() && "a stored row is zero");
        assert(reason_ids.size() <= basis.cols() && "the root conflict walk did not terminate");
        const ResolvedReason reason =
            resolve_asserting_basis(basis, 0, *pivot_col_idx, trail, clauses, workspace,
                                    representative_rng);
        reason_ids.push_back(reason.clause_id);
    }

    // Derivation order to replay order. Exactly one reversal, here.
    std::reverse(reason_ids.begin(), reason_ids.end());
    return reason_ids;
}

AssertingClause find_asserting_clause(const Trail& trail, const std::vector<Clause>& clauses,
                                      ClauseId conflict_clause_id, DecisionProcedure& decisions,
                                      Rng& representative_rng, bool minimize,
                                      bool measure_reason_lbds,
                                      bool report_responsible_trail_idxs, bool collect_subsumed) {
    assert(trail.span().rows() > 0 && "a conflicting trail cannot be empty");
    assert(trail.level() > 0 && "a root conflict is UNSAT and is not analysed");
    assert(trail_is_conflicting(trail) &&
           "conflict analysis on a trail that does not contain 0 = 1");
    assert(conflict_clause_id < clauses.size() && "conflicting reason id is out of range");
    assert(trail.reasons().back() == std::optional<ClauseId>{conflict_clause_id} &&
           "the conflicting clause did not propagate the last trail row");

    std::vector<ClauseId> reason_ids{conflict_clause_id};

    TrailBasisSpace asserting_basis =
        TrailBasisSpace::from_RowSpace(clauses[conflict_clause_id], trail);
    const std::size_t conflict_clause_width = asserting_basis.rows();

    std::vector<std::size_t> reason_lbds;
    if (measure_reason_lbds) {
        reason_lbds.push_back(asserting_basis.literal_block_distance());
    }

    ResolutionWorkspace workspace(trail);

    const auto to_clause = [&trail](const TrailBasisSpace& basis) {
        Clause clause(trail.repr(), trail.num_vars());
        basis.add_rows_to(clause);
        return clause;
    };

    std::vector<SubsumedReason> subsumed_reasons;
    const auto record_replacement = [&](const TrailBasisSpace& basis, ClauseId clause_id,
                                        std::size_t trail_idx, std::size_t num_reasons,
                                        Clause replacement, bool conflicting) {
        subsumed_reasons.push_back(SubsumedReason{clause_id, std::move(replacement),
                                                  basis.literal_block_distance(),
                                                  num_reasons, trail_idx, conflicting});
    };

    // The first accepted resolution is the one step whose left operand is also a
    // stored clause: the conflicting clause. Its rank dropping to one less means
    // the resolvent subsumes it too, exactly as for the reason side.
    bool conflicting_clause_check_open = true;
    const auto maybe_record_subsumed_conflicting = [&]() {
        if (conflicting_clause_check_open && asserting_basis.rows() + 1 == conflict_clause_width) {
            const std::size_t conflict_trail_idx = trail.span().rows() - 1;
            record_replacement(asserting_basis, conflict_clause_id, conflict_trail_idx,
                               reason_ids.size(), to_clause(asserting_basis), true);
        }
        conflicting_clause_check_open = false;
    };

    // The records the latest accepted resolution made. The walk ends on that
    // resolvent, so the clause as learned is their replacement, and no copy is
    // added; harvested records after them resolved to other clauses.
    std::size_t last_resolution_records_begin = 0;
    std::size_t last_resolution_records_end = 0;

    // What both walks do once a resolution is accepted.
    const auto accept_resolution = [&](const ResolvedReason& reason,
                                       std::size_t trail_idx) {
        reason_ids.push_back(reason.clause_id);
        if (measure_reason_lbds) {
            reason_lbds.push_back(*reason.lbd);
        }
        decisions.update_with_resolved_clause(reason.clause_id);
        if (collect_subsumed) {
            last_resolution_records_begin = subsumed_reasons.size();
            maybe_record_subsumed_conflicting();
            if (reason.subsumes_reason) {
                record_replacement(asserting_basis, reason.clause_id, trail_idx,
                                   reason_ids.size(), to_clause(asserting_basis), false);
            }
            last_resolution_records_end = subsumed_reasons.size();
        }
    };

    // The 1-UIP walk: resolve on the latest trail element the clause still uses
    // until it asserts. The pivot always leads row 0 here, so no row is free.
    // The trail is at the conflict's own level, so this reads against that.
    std::size_t resolution_steps = 0;
    while (asserting_basis.asserting_level() == trail.level()) {
        const std::optional<std::size_t> pivot_col_idx = asserting_basis.row(0).leading_bit();
        assert(pivot_col_idx.has_value() && "a stored row is zero");

        ++resolution_steps;
        assert(resolution_steps < asserting_basis.cols() && "conflict analysis did not terminate");
        if (resolution_steps == 1) {
            // The other operand of the first resolution, resolved like any reason.
            // A conflicting clause that already asserts is resolved with nothing.
            decisions.update_with_resolved_clause(conflict_clause_id);
        }
        const ResolvedReason reason =
            resolve_asserting_basis(asserting_basis, 0, *pivot_col_idx, trail, clauses,
                                    workspace, representative_rng, measure_reason_lbds);
        assert(asserting_basis.rows() > 0 && "resolution derived the empty clause below the root");
        accept_resolution(reason, asserting_basis.cols() - 1 - *pivot_col_idx);
    }

    const std::size_t width_before_minimize = asserting_basis.rows();

    // Minimization: continue the walk to the start of the trail, accepting a
    // resolution only when it strictly shrinks the clause; decision-led rows are
    // kept. Only an accepted step reaches the heuristics and the proof.
    //
    // `frozen` counts the leading rows the walk has passed over. They keep their
    // leading columns, strictly earlier than the pivot, though a resolution may
    // add the pivot row into them.
    for (std::size_t frozen = 0; minimize && frozen < asserting_basis.rows();) {
        const std::optional<std::size_t> pivot_col_idx = asserting_basis.row(frozen).leading_bit();
        assert(pivot_col_idx.has_value() && "a stored row is zero");

        const std::size_t trail_idx = asserting_basis.cols() - 1 - *pivot_col_idx;
        assert(trail_idx < trail.reasons().size() && "trail reason index is out of range");
        if (trail.reasons()[trail_idx].has_value()) {
            const ClauseId reason_clause_id = *trail.reasons()[trail_idx];
            const TrailCoordinates& reason_coordinates =
                TrailBasisSpace::reason_coordinates(clauses[reason_clause_id], trail, trail_idx);
            const bool narrows = resolution_narrows(asserting_basis, frozen, reason_coordinates,
                                                    trail_idx, workspace);
#ifndef NDEBUG
            {
                Rng rng_copy = representative_rng;
                TrailBasisSpace resolved = asserting_basis;
                resolve_asserting_basis(resolved, frozen, *pivot_col_idx, trail, clauses,
                                        workspace, rng_copy, false);
                assert(narrows == (resolved.rows() < asserting_basis.rows()) &&
                       "the narrowing test disagrees with the resolution");
            }
#endif
            if (narrows) {
                // Resolution keeps every row before the pivot and drops only the
                // pivot row, so the resolvent is empty only from a one-row clause.
                // Learn that unit rather than the empty clause: it asserts at the
                // root, where whatever made the resolvent empty contradicts it on
                // the next propagation and the run ends UNSAT through the usual path.
                if (asserting_basis.rows() == 1) {
                    break;
                }
                const ResolvedReason reason = resolve_asserting_basis(
                    asserting_basis, frozen, *pivot_col_idx, trail, clauses, workspace,
                    representative_rng, measure_reason_lbds);
                accept_resolution(reason, trail_idx);
                continue;
            }
            // The discarded resolvent keeps every row of the clause but the pivot
            // row and gains one, so it can subsume its reason only if the reason is
            // wider than the clause. Then it still does, so harvest the replacement
            // anyway; the walk keeps the wider clause. Its chain is one longer than
            // `reason_ids`, which never received this reason.
            if (collect_subsumed && reason_coordinates.rows() > asserting_basis.rows()) {
                TrailBasisSpace resolved = asserting_basis;
                const ResolvedReason reason = resolve_asserting_basis(
                    resolved, frozen, *pivot_col_idx, trail, clauses, workspace,
                    representative_rng, measure_reason_lbds);
                if (reason.subsumes_reason) {
                    record_replacement(resolved, reason.clause_id, trail_idx,
                                       reason_ids.size() + 1, to_clause(resolved), false);
                }
            } else {
                // The coins the discarded resolution would have drawn.
                representative_rng.discard((frozen + 63) / 64);
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

    std::vector<std::size_t> responsible_trail_idxs;
    if (report_responsible_trail_idxs) {
        responsible_trail_idxs.reserve(asserting_basis.rows());
        for (const Equation& row : asserting_basis) {
            assert(row.leading_bit().has_value() && "a stored row is zero");
            responsible_trail_idxs.push_back(asserting_basis.cols() - 1 - *row.leading_bit());
        }
    }

    // The clause as learned is those records' replacement; the caller learns it
    // rather than adding a copy.
    for (std::size_t i = last_resolution_records_begin; i < last_resolution_records_end; ++i) {
        subsumed_reasons[i].replacement.reset();
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
                          std::move(responsible_trail_idxs),
                          std::move(subsumed_reasons)};
}

} // namespace xorcle
