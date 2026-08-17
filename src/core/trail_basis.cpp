#include "core/trail_basis.h"

#include <cassert>
#include <cstddef>
#include <optional>
#include <vector>

#include "util/bitmap.h"

namespace xorcle {

namespace {

#ifndef NDEBUG
/// @brief `Trail` asserts its order maps as it builds them; the check is
/// repeated at the point of *use* because every column index below is computed
/// through them.
bool trail_maps_are_consistent(const Trail& trail) {
    const std::size_t trail_rows = trail.span().rows();
    if (trail.order_map().size() != trail_rows || trail.reverse_order_map().size() != trail_rows ||
        trail.reasons().size() != trail_rows) {
        return false;
    }
    for (std::size_t trail_index = 0; trail_index < trail_rows; ++trail_index) {
        const std::size_t row_index = trail.order_map()[trail_index];
        if (row_index >= trail_rows || trail.reverse_order_map()[row_index] != trail_index) {
            return false;
        }
    }
    return true;
}
#endif

} // namespace

TrailBasisSpace TrailBasisSpace::from_RowSpace(const RowSpace& rs, const Trail& trail) {
    assert(trail.span().rows() != 0 && "trail span is empty");
    assert(rs.cols() == trail.span().cols() && "row space width does not match the trail span");
    assert(rs.backend() == trail.backend() && "row space and trail use different backends");
    assert(trail_maps_are_consistent(trail));

    TrailBasisSpace trail_basis(trail);

    // The span's row count is both the coefficient width and this matrix's
    // column count -- the point of the `m - 1` in the constructor.
    const std::size_t coefficient_width = trail.span().rows();
    assert(trail_basis.cols() == coefficient_width);

    // Preallocated scratch: nothing in the loop allocates after the first pass.
    Equation reduced_eqn(trail.backend(), trail.num_vars());
    Equation trail_basis_row(trail.backend(), trail_basis.num_vars());
    Bitmap row_coefficients;
    std::vector<std::size_t> mapped_columns;

    for (const Equation& eqn : rs) {
        assert(eqn.size() == reduced_eqn.size());
        reduced_eqn.assign_from(eqn);
        // `row_coefficients` is indexed by row index within the span, i.e.
        // row-echelon order, not trail order.
        trail.span().reduce(reduced_eqn, row_coefficients);
        assert(reduced_eqn.is_eq_true() && "row space contains a row outside the trail span");
        assert(row_coefficients.size() == coefficient_width);

        // Rewriting into trail-basis coordinates permutes the bit vector by a
        // fixed index map: span row index -> trail order (`reverse_order_map`)
        // -> reversed (`m - 1 - t`), collected once and handed to the algebra
        // layer in a single `assign_columns()` dispatch.
        mapped_columns.clear();
        row_coefficients.for_each_set([&](std::size_t row_index) {
            mapped_columns.push_back(coefficient_width - trail.reverse_order_map()[row_index] - 1);
        });
        trail_basis_row.assign_columns(mapped_columns);

        // `add_row()` echelonizes into ascending leading-column order, i.e. most
        // recent trail equation first.
        trail_basis.add_row(trail_basis_row);
    }

    return trail_basis;
}

RowSpace TrailBasisSpace::to_RowSpace() const {
    assert(trail_maps_are_consistent(trail_));

    RowSpace row_space(trail_.backend(), trail_.num_vars());
    Equation lifted_eqn(trail_.backend(), trail_.num_vars());
    std::vector<std::size_t> coefficient_columns;
    const std::size_t coefficient_width = cols();

    for (const Equation& trail_basis_row : *this) {
        // XOR-accumulate the selected subset of trail rows, dispatching at row
        // granularity: one `columns()` call, then one `xor_assign()` per row.
        lifted_eqn.clear();
        trail_basis_row.columns(coefficient_columns);
        for (const std::size_t coefficient_column : coefficient_columns) {
            const std::size_t trail_index = coefficient_width - coefficient_column - 1;
            lifted_eqn.xor_assign(trail_.span().row(trail_.order_map()[trail_index]));
        }
        row_space.add_row(lifted_eqn);
    }

    return row_space;
}

std::size_t TrailBasisSpace::asserting_level() const {
    assert(rows() > 0 && "asserting level of an empty trail-basis space");

    // A single-row trail-basis space is a lifted unit clause: it asserts at the
    // root, so every non-root level can be discarded.
    if (rows() == 1) {
        return 0;
    }

    // Only row 1 is ever examined. Row 0 is about to be resolved away, and rows
    // are stored in strictly increasing leading-column order, so if row 1 has no
    // set column below some limit then neither do rows 2, 3, ... . Row 1 alone
    // therefore decides how far back the remaining basis reaches. And for a
    // single row, "has a set column below the limit" is exactly
    // `leading_column() < limit`, so no sub-vector need be materialized.
    const std::optional<std::size_t> second_row_leading = row(1).leading_column();
    assert(second_row_leading && "a stored row is zero");

    const std::size_t span_rows = trail_.span().rows();
    for (std::size_t candidate_level = trail_.level(); candidate_level > 0;) {
        --candidate_level;

        // Equations learned strictly after `candidate_level` occupy trail steps
        // `[trail_[candidate_level].span().rows(), span_rows)`, i.e. columns
        // `[0, current_level_width)` of this matrix.
        const std::size_t current_level_width =
            span_rows - trail_[candidate_level].span().rows();

        // Row 1 reaches into a level proposed for discarding, so the clause
        // asserts one level above `candidate_level`.
        if (*second_row_leading < current_level_width) {
            return candidate_level + 1;
        }
    }

    // Row 1 never reached past the root, so every non-root level goes.
    return 0;
}

} // namespace xorcle
