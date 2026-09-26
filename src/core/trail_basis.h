#pragma once

#include <cstddef>
#include <optional>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/trail.h"

namespace xorcle {

/// @brief A matrix representing a subspace of the trail span, where the columns
/// represent `[0 = 1]`, @f$f_m, \dots, f_1@f$ respectively, instead of
/// @f$x_1, \dots, x_n@f$, `[0 = 1]`.
///
/// The reverse level-major ordering of columns with row echelon form
/// automatically performs the change of basis necessary to isolate the latest
/// used equation, so that it can be eliminated by addition. See
/// arXiv:2605.15002 for details.
///
/// @note Learning or backjumping invalidates an already-built `TrailBasisSpace`.
class TrailBasisSpace : public RowEchelonMatrix {
public:
    explicit TrailBasisSpace(const Trail& trail)
        : RowEchelonMatrix(trail.repr(), trail.span().rows()), trail_(&trail) {}

    /// @brief Rewrite a row space into trail-basis coordinates, echelonized.
    /// @pre Every row of `rs` belongs to `trail.span()`, which is non-empty.
    /// @pre `rs.cols() == trail.span().cols()` and `rs.repr() == trail.repr()`.
    static TrailBasisSpace from_RowSpace(const RowSpace& rs, const Trail& trail);

    /// @brief The reason clause of the row at `trail_idx` written as sums of trail
    /// rows, stored on the trail by the first conflict that asks.
    /// @pre `reason` is that row's reason clause, and the trail conflicts.
    static const TrailCoordinates& reason_coordinates(const RowSpace& reason, const Trail& trail,
                                                      std::size_t trail_idx);

    /// @brief `from_RowSpace()` of the reason clause of the row at `trail_idx`.
    /// @pre As `reason_coordinates()`.
    static TrailBasisSpace from_reason(const RowSpace& reason, const Trail& trail,
                                       std::size_t trail_idx);

    /// @brief Row `row_idx` of `coordinates` in these columns, reading any index
    /// past `last_trail_idx` as the conflict's `0 = 1` in column 0.
    /// @pre `row_idx < coordinates.rows()`.
    Equation coordinates_row(const TrailCoordinates& coordinates, std::size_t row_idx,
                             std::size_t last_trail_idx) const;

    /// @brief Convert back out of trail-basis coordinates.
    ///
    /// Round-trips through `from_RowSpace()` preserve the *span*, not the rows.
    /// @pre The trail has not been learned into or backjumped since.
    RowSpace to_RowSpace() const;

    /// @brief The level to backjump to, in `[0, trail.level()]`: `0` is the root,
    /// `trail.level()` means nothing can be discarded.
    /// @pre `rows() > 0`.
    /// @note `O(trail.level())`.
    std::size_t asserting_level() const;

    /// @brief How many dimensions these rows span at the top decision level,
    /// i.e. their dimension modulo the levels below it.
    /// @pre `rows() > 0`.
    /// @note `O(cols())`.
    std::size_t dimension_at_top_level() const;

    /// @brief How many decision levels above the root these rows reach into.
    ///
    /// The literal block distance of the clause they negate: the trail prefixes
    /// meet this subspace exactly at its pivot columns, so those columns stand
    /// in for the literals counted in the Boolean case. Root pivots count for
    /// nothing, as root literals do.
    /// @pre `rows() > 0`.
    /// @note `O(rows() + trail.level())`.
    std::size_t literal_block_distance() const;

    /// @brief `literal_block_distance()` with column `col_idx` counted as one more
    /// pivot.
    /// @pre `col_idx < cols()`.
    /// @note `O(rows() + trail.level())`.
    std::size_t literal_block_distance_with(std::size_t col_idx) const;

private:
    /// @brief The number of columns holding equations learned strictly after
    /// `level`, which are the columns below the returned index.
    /// @pre `level <= trail.level()`.
    std::size_t columns_after_level(std::size_t level) const;

    /// @brief The levels above the root holding a pivot column or `extra_col_idx`.
    std::size_t levels_reached(std::optional<std::size_t> extra_col_idx) const;

    /// @brief Each row of `rs` as the trail indices of the span rows summing to it.
    static TrailCoordinates coordinates_of(const RowSpace& rs, const Trail& trail);

    /// @brief The trail-basis space of `coordinates`, read as `coordinates_row()` does.
    static TrailBasisSpace from_coordinates(const TrailCoordinates& coordinates,
                                            const Trail& trail, std::size_t last_trail_idx);

    const Trail* trail_;
};

} // namespace xorcle
