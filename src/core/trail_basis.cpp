#include "core/trail_basis.h"

#include <cassert>
#include <cstddef>
#include <optional>
#include <vector>

#include "util/bitmap.h"

namespace xorcle {

namespace {

#ifndef NDEBUG
/// @brief Whether a trail snapshot's two order maps are consistent.
///
/// The 0.1 tree checked this with a throwing `validate_trail_snapshot_or_throw`
/// on entry to both conversions. `Trail` maintains the maps itself and already
/// asserts them as it builds them, so a violation here is an internal invariant
/// failure, never bad user input, so it is an
/// assert. The check is kept at the point of *use* as well as the point of
/// construction because every column index below is computed through these maps.
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

    // The row count of the span is both the coefficient width and this matrix's
    // column count, which is the whole point of the `m - 1` in the constructor.
    const std::size_t coefficient_width = trail.span().rows();
    assert(trail_basis.cols() == coefficient_width);

    // Preallocated scratch, refilled per row: nothing in this loop allocates
    // after the first iteration.
    Equation reduced_eqn(trail.backend(), trail.num_vars());
    Equation trail_basis_row(trail.backend(), trail_basis.num_vars());
    Bitmap row_coefficients;
    std::vector<std::size_t> mapped_columns;

    for (const Equation& eqn : rs) {
        assert(eqn.size() == reduced_eqn.size());
        reduced_eqn.assign_from(eqn);
        // `row_coefficients` is indexed by *row index within the span*, which is
        // row-echelon order, not trail order.
        trail.span().reduce(reduced_eqn, row_coefficients);
        assert(reduced_eqn.is_eq_true() && "row space contains a row outside the trail span");
        assert(row_coefficients.size() == coefficient_width);

        // Rewriting the coefficients into trail-basis coordinates is a
        // permutation of a bit vector by a fixed index map: span row index ->
        // trail order (`reverse_order_map`) -> reversed (`m - 1 - t`). Collect
        // the mapped columns once, then hand the algebra layer the whole row in
        // a single `assign_columns()` dispatch. The 0.1 tree did this one bit at
        // a time, which is exactly the per-bit access the new algebra interface
        // refuses to expose.
        mapped_columns.clear();
        row_coefficients.for_each_set([&](std::size_t row_index) {
            mapped_columns.push_back(coefficient_width - trail.reverse_order_map()[row_index] - 1);
        });
        trail_basis_row.assign_columns(mapped_columns);

        // `add_row()` echelonizes, so the rows of the result end up in ascending
        // leading-column order, i.e. most recent trail equation first.
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
        // XOR-accumulate the selected subset of trail rows. Dispatch is at row
        // granularity -- one `columns()` call to learn which rows are selected,
        // then one `xor_assign()` per selected row -- so no per-bit access
        // crosses the algebra boundary and nothing is returned by value.
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
    // root, so every non-root level can be discarded. In the 0.1 tree's sign
    // convention this was `return trail.size() - 1`.
    if (rows() == 1) {
        return 0;
    }

    // Only row 1 is ever examined. Row 0 is the row conflict analysis is about
    // to resolve away, and rows are stored in strictly increasing leading-column
    // order, so if row 1 has no set column below some limit then neither do rows
    // 2, 3, ... . Row 1 alone therefore decides how far back the whole remaining
    // basis reaches.
    const std::optional<std::size_t> second_row_leading = row(1).leading_column();
    assert(second_row_leading && "a stored row is zero");

    // The 0.1 tree wrote this test as `row(1).sub(current_level_width).any()`.
    // Despite the local being named `second_row_suffix` there, `sub(len)`
    // returns the *first* `len` bits: a prefix in column index, which is a
    // suffix in trail order, because column `c` is trail step `m - 1 - c`. For a
    // single row, "has a set column below `limit`" is precisely "its smallest
    // set column is below `limit`", i.e. `leading_column() < limit` -- so the
    // whole sub-vector allocation collapses into one comparison, with no new
    // algebra operation and no change of meaning.
    const std::size_t span_rows = trail_.span().rows();
    for (std::size_t candidate_level = trail_.level(); candidate_level > 0;) {
        --candidate_level;

        // Equations learned strictly after `candidate_level` occupy trail steps
        // `[trail_[candidate_level].span().rows(), span_rows)`, which is
        // precisely columns `[0, current_level_width)` of this matrix.
        const std::size_t current_level_width =
            span_rows - trail_[candidate_level].span().rows();

        // Row 1 reaches into a level we proposed to discard, so `candidate_level`
        // is one level too deep: the clause asserts at the level above it. The
        // 0.1 tree returned `removable_levels - 1` here, with
        // `removable_levels == trail.level() - candidate_level`.
        if (*second_row_leading < current_level_width) {
            return candidate_level + 1;
        }
    }

    // Row 1 never reached past the root, so every non-root level goes. The 0.1
    // tree returned `trail.size() - 1` on this path.
    return 0;
}

} // namespace xorcle
