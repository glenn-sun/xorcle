#include "core/trail_basis.h"

#include <cassert>
#include <cstddef>
#include <optional>
#include <vector>

#include "util/index_set.h"

namespace xorcle {

TrailCoordinates TrailBasisSpace::coordinates_of(const RowSpace& rs, const Trail& trail) {
    assert(trail.span().rows() != 0 && "trail span is empty");
    assert(rs.cols() == trail.span().cols() &&
           "row space column count does not match the trail span");
    assert(rs.repr() == trail.repr() && "row space and trail use different representations");

    TrailCoordinatesWorkspace& workspace = trail.coordinates_workspace();
    if (workspace.reduced.is_null()) {
        workspace.reduced = Equation(trail.repr(), trail.num_vars());
    }
    workspace.trail_idxs.clear();

    TrailCoordinates coordinates;
    coordinates.row_ends.reserve(rs.rows());
    for (const Equation& eqn : rs) {
        workspace.reduced.assign_from(eqn);
        // Indexed by row-echelon order within the span, not trail order.
        trail.span().reduce_get_coefficients(workspace.reduced, workspace.row_coefficients);
        assert(workspace.reduced.is_eq_true() &&
               "row space contains a row outside the trail span");
        workspace.row_coefficients.for_each_set([&](std::size_t row_idx) {
            workspace.trail_idxs.push_back(trail.row_idx_to_trail_idx()[row_idx]);
        });
        coordinates.row_ends.push_back(workspace.trail_idxs.size());
    }
    coordinates.trail_idxs = workspace.trail_idxs;
    return coordinates;
}

void TrailBasisSpace::assign_coordinates_row(Equation& row, const TrailCoordinates& coordinates,
                                             std::size_t row_idx,
                                             std::size_t last_trail_idx) const {
    assert(row_idx < coordinates.rows() && "coordinates row index is out of range");
    assert(row.num_bits() == cols() && "the row is not over the trail basis");
    const std::size_t row_begin = row_idx == 0 ? 0 : coordinates.row_ends[row_idx - 1];
    const std::size_t row_end = coordinates.row_ends[row_idx];

    // Trail order reversed.
    std::vector<std::size_t>& mapped_bits = trail_->coordinates_workspace().mapped_bits;
    mapped_bits.clear();
    for (std::size_t i = row_begin; i < row_end; ++i) {
        const std::size_t trail_idx = coordinates.trail_idxs[i];
        mapped_bits.push_back(trail_idx > last_trail_idx ? 0 : cols() - trail_idx - 1);
    }
    row.assign_bits(mapped_bits);
}

void TrailBasisSpace::add_coordinates_rows(const TrailCoordinates& coordinates,
                                           std::size_t last_trail_idx,
                                           EquationPool& pool) {
    for (std::size_t row_idx = 0; row_idx < coordinates.rows(); ++row_idx) {
        Equation row = pool.take();
        assign_coordinates_row(row, coordinates, row_idx, last_trail_idx);
        // Echelonized into ascending leading-column order, i.e. most recent
        // trail equation first.
        if (!add_row_or_keep(row).has_value()) {
            pool.give(std::move(row));
        }
    }
}

TrailBasisSpace TrailBasisSpace::from_coordinates(const TrailCoordinates& coordinates,
                                                  const Trail& trail,
                                                  std::size_t last_trail_idx) {
    TrailBasisSpace trail_basis(trail);
    EquationPool pool(trail_basis.repr(), trail_basis.cols());
    trail_basis.add_coordinates_rows(coordinates, last_trail_idx, pool);
    return trail_basis;
}

TrailBasisSpace TrailBasisSpace::from_RowSpace(const RowSpace& rs, const Trail& trail) {
    return from_coordinates(coordinates_of(rs, trail), trail, trail.span().rows() - 1);
}

const TrailCoordinates& TrailBasisSpace::reason_coordinates(const RowSpace& reason,
                                                            const Trail& trail,
                                                            std::size_t trail_idx) {
    assert(trail_idx + 1 < trail.span().rows() && "the reason's row is the conflict's or later");
    const TrailCoordinates* stored = trail.reason_coordinates(trail_idx);
    if (stored == nullptr) {
        trail.store_reason_coordinates(trail_idx, coordinates_of(reason, trail));
        stored = trail.reason_coordinates(trail_idx);
    }
    // A reason lies in the span of the rows up to its own and `0 = 1`, and the
    // rows it names have not changed since.
    assert(from_coordinates(*stored, trail, trail_idx).has_rows_of(from_RowSpace(reason, trail)) &&
           "the stored reason coordinates are stale");
    return *stored;
}

void TrailBasisSpace::add_reason_rows(const RowSpace& reason, std::size_t trail_idx,
                                      EquationPool& pool) {
    assert(pool.repr() == repr() && pool.num_bits() == cols() &&
           "add_reason_rows: the pool's representation or bit count differs");
    add_coordinates_rows(reason_coordinates(reason, *trail_, trail_idx), trail_idx, pool);
}

void TrailBasisSpace::add_rows_to(RowSpace& row_space) const {
    assert(row_space.cols() == trail_->span().cols() && row_space.repr() == trail_->repr() &&
           "the row space is not over the trail's variables");

    Equation row_space_eqn(trail_->repr(), trail_->num_vars());
    const std::size_t num_coefficients = cols();

    for (const Equation& trail_basis_row : *this) {
        row_space_eqn.set_eq_true();
        for (const std::size_t coefficient_bit : trail_basis_row.bits()) {
            const std::size_t trail_idx = num_coefficients - coefficient_bit - 1;
            row_space_eqn.plus_assign(trail_->row(trail_idx));
        }
        row_space.add_row(row_space_eqn);
    }
}

std::size_t TrailBasisSpace::columns_after_level(std::size_t level) const {
    assert(level <= trail_->level() && "decision level is out of range");
    return trail_->span().rows() - trail_->rows_at_level(level);
}

std::size_t TrailBasisSpace::asserting_level() const {
    assert(rows() > 0 && "asserting level of an empty trail-basis space");

    // A single-row basis converts back to a unit clause, which asserts at the root.
    if (rows() == 1) {
        return 0;
    }

    // Row 0 is about to be resolved away, and rows are in strictly increasing
    // leading-column order, so row 1 alone decides how far back the basis reaches.
    const std::optional<std::size_t> second_row_leading = row(1).leading_bit();
    assert(second_row_leading && "a stored row is zero");

    for (std::size_t candidate_level = trail_->level(); candidate_level > 0;) {
        --candidate_level;
        if (*second_row_leading < columns_after_level(candidate_level)) {
            return candidate_level + 1;
        }
    }

    return 0;
}

std::size_t TrailBasisSpace::dimension_at_top_level() const {
    assert(rows() > 0 && "dimension at the top level of an empty trail-basis space");

    // The top level owns the columns before the equations at the level below it,
    // and the root level owns them all. Rows lead in strictly ascending columns,
    // so the ones leading there are exactly what survives the levels below.
    const std::size_t level = trail_->level();
    const std::size_t block_end =
        level == 0 ? trail_->span().rows() : columns_after_level(level - 1);
    return pivot_col_idxs().count_below(block_end);
}

std::size_t TrailBasisSpace::literal_block_distance() const {
    assert(rows() > 0 && "literal block distance of an empty trail-basis space");
    return levels_reached(std::nullopt);
}

std::size_t TrailBasisSpace::literal_block_distance_with(std::size_t col_idx) const {
    assert(col_idx < cols() && "the extra column is out of range");
    return levels_reached(col_idx);
}

std::size_t TrailBasisSpace::levels_reached(std::optional<std::size_t> extra_col_idx) const {
    // The columns a level owns end where the next level up begins, and the root
    // level runs to the last column.
    const auto block_end_of = [this](std::size_t level) {
        return level == 0 ? trail_->span().rows() : columns_after_level(level - 1);
    };

    // Columns ascend into the past exactly as levels descend, so one ascending
    // walk enters each level's block at most once.
    std::size_t level = trail_->level();
    std::size_t block_end = block_end_of(level);
    std::size_t blocks = 0;
    bool block_counted = false;
    const auto visit = [&](std::size_t col_idx) {
        while (col_idx >= block_end) {
            assert(level > 0 && "a column lies past the root level");
            --level;
            block_end = block_end_of(level);
            block_counted = false;
        }
        if (!block_counted && level > 0) {
            ++blocks;
            block_counted = true;
        }
    };

    pivot_col_idxs().for_each_set([&](std::size_t col_idx) {
        if (extra_col_idx.has_value() && *extra_col_idx <= col_idx) {
            visit(*extra_col_idx);
            extra_col_idx.reset();
        }
        visit(col_idx);
    });
    if (extra_col_idx.has_value()) {
        visit(*extra_col_idx);
    }

    return blocks;
}

} // namespace xorcle
