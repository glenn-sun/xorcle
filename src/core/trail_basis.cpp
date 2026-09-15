#include "core/trail_basis.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <optional>
#include <vector>

#include "util/index_set.h"

namespace xorcle {

namespace {

#ifndef NDEBUG
bool trail_maps_are_consistent(const Trail& trail) {
    const std::size_t trail_rows = trail.span().rows();
    if (trail.order_map().size() != trail_rows || trail.reverse_order_map().size() != trail_rows ||
        trail.reasons().size() != trail_rows) {
        return false;
    }
    for (std::size_t trail_index = 0; trail_index < trail_rows; ++trail_index) {
        const std::size_t row_idx = trail.order_map()[trail_index];
        if (row_idx >= trail_rows || trail.reverse_order_map()[row_idx] != trail_index) {
            return false;
        }
    }
    return true;
}
#endif

} // namespace

TrailBasisSpace TrailBasisSpace::from_RowSpace(const RowSpace& rs, const Trail& trail,
                                               std::vector<std::size_t>* row_leading_cols) {
    assert(trail.span().rows() != 0 && "trail span is empty");
    assert(rs.cols() == trail.span().cols() &&
           "row space column count does not match the trail span");
    assert(rs.repr() == trail.repr() && "row space and trail use different representations");
    assert(trail_maps_are_consistent(trail) && "the trail's order maps are inconsistent");

    TrailBasisSpace trail_basis(trail);

    const std::size_t num_coefficients = trail_basis.cols();
    if (row_leading_cols != nullptr) {
        row_leading_cols->clear();
    }

    Equation reduced_eqn(trail.repr(), trail.num_vars());
    Equation trail_basis_row = Equation::with_num_bits(trail.repr(), num_coefficients);
    std::vector<std::size_t> mapped_bits;

    for (const Equation& eqn : rs) {
        reduced_eqn.assign_from(eqn);
        // Indexed by row-echelon order within the span, not trail order.
        const IndexSet row_coefficients = trail.span().reduce_get_coefficients(reduced_eqn);
        assert(reduced_eqn.is_eq_true() && "row space contains a row outside the trail span");
        assert(row_coefficients.size() == num_coefficients);

        // Span row index -> trail order (`reverse_order_map`) -> reversed.
        mapped_bits.clear();
        row_coefficients.for_each_set([&](std::size_t row_idx) {
            mapped_bits.push_back(num_coefficients - trail.reverse_order_map()[row_idx] - 1);
        });
        trail_basis_row.assign_bits(mapped_bits);
        if (row_leading_cols != nullptr) {
            assert(!mapped_bits.empty() && "a row space row is zero");
            row_leading_cols->push_back(
                *std::min_element(mapped_bits.begin(), mapped_bits.end()));
        }

        // Echelonized into ascending leading-column order, i.e. most recent
        // trail equation first.
        trail_basis.add_row(trail_basis_row);
    }

    return trail_basis;
}

RowSpace TrailBasisSpace::to_RowSpace() const {
    assert(trail_maps_are_consistent(*trail_) && "the trail's order maps are inconsistent");

    RowSpace row_space(trail_->repr(), trail_->num_vars());
    Equation row_space_eqn(trail_->repr(), trail_->num_vars());
    const std::size_t num_coefficients = cols();

    for (const Equation& trail_basis_row : *this) {
        row_space_eqn.set_eq_true();
        for (const std::size_t coefficient_bit : trail_basis_row.bits()) {
            const std::size_t trail_index = num_coefficients - coefficient_bit - 1;
            row_space_eqn.plus_assign(trail_->span().row(trail_->order_map()[trail_index]));
        }
        row_space.add_row(row_space_eqn);
    }

    return row_space;
}

std::size_t TrailBasisSpace::columns_after_level(std::size_t level) const {
    assert(level <= trail_->level() && "decision level is out of range");
    return trail_->span().rows() - (*trail_)[level].span().rows();
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

std::size_t TrailBasisSpace::literal_block_distance() const {
    assert(rows() > 0 && "literal block distance of an empty trail-basis space");

    // The columns a level owns end where the next level up begins, and the root
    // level runs to the last column.
    const auto block_end_of = [this](std::size_t level) {
        return level == 0 ? trail_->span().rows() : columns_after_level(level - 1);
    };

    // Columns ascend into the past exactly as levels descend, so one walk over
    // the pivot columns enters each level's block at most once.
    std::size_t level = trail_->level();
    std::size_t block_end = block_end_of(level);
    std::size_t blocks = 0;
    bool block_counted = false;

    pivot_col_idxs().for_each_set([&](std::size_t col_idx) {
        while (col_idx >= block_end) {
            assert(level > 0 && "a pivot column lies past the root level");
            --level;
            block_end = block_end_of(level);
            block_counted = false;
        }
        if (!block_counted) {
            ++blocks;
            block_counted = true;
        }
    });

    return blocks;
}

} // namespace xorcle
