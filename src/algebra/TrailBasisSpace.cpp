#include "algebra/TrailBasisSpace.h"
#include "search/Trail.h"
#include <cassert>
#include <stdexcept>

namespace algebra {

namespace {

void validate_trail_snapshot_or_throw(const search::Trail& trail, const char* where) {
    const std::size_t trail_rows = trail.span().rows();
    if (trail.order_map().size() != trail_rows ||
        trail.reverse_order_map().size() != trail_rows ||
        trail.reasons().size() != trail_rows) {
        throw std::logic_error(std::string(where) + ": trail snapshot arrays are out of sync");
    }

    for (std::size_t trail_index = 0; trail_index < trail_rows; ++trail_index) {
        const std::size_t row_index = trail.order_map()[trail_index];
        if (row_index >= trail_rows) {
            throw std::logic_error(std::string(where) + ": trail order map contains an out-of-range row index");
        }
        if (trail.reverse_order_map()[row_index] != trail_index) {
            throw std::logic_error(std::string(where) + ": trail maps are not inverse permutations");
        }
    }
}

} // namespace

TrailBasisSpace TrailBasisSpace::from_RowSpace(const RowSpace& rs, const search::Trail& trail) {
    if (trail.span().rows() == 0) {
        throw std::invalid_argument("TrailBasisSpace::from_RowSpace: trail span is empty");
    }
    if (rs.cols() != trail.span().cols()) {
        throw std::invalid_argument("TrailBasisSpace::from_RowSpace: row space width does not match trail span");
    }
    validate_trail_snapshot_or_throw(trail, "TrailBasisSpace::from_RowSpace");

    TrailBasisSpace tbs(trail);
    Coefficients coeffs;
    Equation reduced_eqn{trail.span().cols() - 1};
    for (const auto& eqn : rs) {
        if (reduced_eqn.size() != eqn.size()) {
            throw std::logic_error("TrailBasisSpace::from_RowSpace: equation width mismatch");
        }
        reduced_eqn.unsafe_copy(eqn);
        trail.span().reduce(reduced_eqn, &coeffs);
        if (reduced_eqn.any()) {
            throw std::invalid_argument("TrailBasisSpace::from_RowSpace: row space contains a row outside trail span");
        }

        const std::size_t coeff_width = coeffs.size();
        Coefficients trail_basis_row = Coefficients::zeros(coeff_width);
        coeffs.if_set_call([&](std::size_t row_index) {
            trail_basis_row.set(coeff_width - trail.reverse_order_map()[row_index] - 1);
        });
        tbs.add_row(trail_basis_row);
    }
    return tbs;
}

RowSpace TrailBasisSpace::to_RowSpace() const {
    validate_trail_snapshot_or_throw(trail, "TrailBasisSpace::to_RowSpace");

    RowSpace row_space(trail.span().cols() - 1);
    Equation lifted_eqn(trail.span().cols() - 1);
    for (const auto& trail_basis_row : *this) {
        lifted_eqn.reset();
        trail_basis_row.if_set_call([&](std::size_t coeff_col) {
            lifted_eqn += trail.span().row(trail.order_map()[cols() - coeff_col - 1]);
        });
        row_space.add_row(lifted_eqn);
    }
    return row_space;
}

std::size_t TrailBasisSpace::jumpable_levels() const {
    if (rows() == 0) {
        throw std::runtime_error("TrailBasisSpace::jumpable_levels: trail-basis space is empty");
    }

    if (rows() == 1) {
        return trail.size() - 1;
    }

    std::size_t removable_levels = 1;
    bit::vector<> second_row_suffix;
    while (removable_levels < trail.size()) {
        const std::size_t earlier_trail_rows = trail[trail.size() - 1 - removable_levels].span.rows();
        const std::size_t current_level_width = trail.span().rows() - earlier_trail_rows;
        second_row_suffix = row(1).sub(current_level_width);
        if (second_row_suffix.any()) {
            return removable_levels - 1;
        }
        ++removable_levels;
    } 
    
    return removable_levels - 1;
}
} // namespace algebra
