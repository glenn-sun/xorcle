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
/// The reverse chronological ordering of columns with row echelon form
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

    /// @brief Convert back out of trail-basis coordinates.
    ///
    /// Round-trips through `from_RowSpace()` preserve the *span*, not the rows.
    /// @pre The trail has not been learned into or backjumped since.
    RowSpace to_RowSpace() const;

    /// @brief The level to backjump to, in `[0, trail.level()]`: `0` is the root,
    /// `trail.level()` means nothing can be discarded.
    /// @pre `rows() > 0`.
    /// @note `O(trail.level())`, called twice per resolution step.
    std::size_t asserting_level() const;

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

    /// @brief Held by pointer, not by reference, so the class stays assignable.
    const Trail* trail_;
};

} // namespace xorcle
