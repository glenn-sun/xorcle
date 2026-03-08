#pragma once

#include <cstddef>

#include "algebra/RowSpace.h"
#include "bit/vector.h"
#include "search/Trail.h"

namespace algebra {

/// @brief A vector of coefficients over GF(2).
///
/// Used when representing equations as linear combinations of other
/// equations. The length of this vector is the dimension of the current
/// subspace, not the number of variables + 1.
class Coefficients : public bit::vector<> {
public:
    /// @brief Construct an empty coefficient vector.
    Coefficients() {}

    /// @brief Construct from an existing bit vector.
    Coefficients(const bit::vector<>& v) : bit::vector<>(v) {}
};

/// @brief A subspace of a trail span, represented as a basis in
/// row-echelon form.
///
/// @note Each column corresponds to an equation in the trail in reverse trail
/// appending order: column 0 is the most recently learned equation.
class TrailBasisSpace : public REFMatrix {
private:
    const search::Trail& trail; ///< Reference to the trail being represented.

public:
    /// @brief Construct a trail-basis space from a trail reference.
    /// @param trail Trail whose learned equations define the coefficient basis.
    /// @note When the trail span has `m` rows, this matrix is configured for coefficient
    /// vectors of width `m`, including the inherited constant column.
    explicit TrailBasisSpace(const search::Trail& trail)
        : REFMatrix(trail.span().rows() > 0 ? trail.span().rows() - 1 : 0), trail(trail) {}

    /// @brief Construct a TrailBasisSpace from a RowSpace w.r.t. a Trail.
    /// @param rs Row space to express in trail-basis coordinates.
    /// @param trail Trail whose span supplies the basis.
    /// @return The rows of `rs` rewritten as coefficient rows in reverse trail order.
    /// @pre `rs.cols() == trail.span().cols()`.
    /// @pre Every row in `rs` belongs to `trail.span()`.
    /// @pre The trail snapshot maps are internally consistent.
    /// @throws std::invalid_argument if the widths differ, the trail is empty, or a row of `rs`
    /// lies outside `trail.span()`.
    /// @throws std::logic_error if the trail snapshot maps are inconsistent.
    static TrailBasisSpace from_RowSpace(const RowSpace& rs, const search::Trail& trail);

    /// @brief Convert this TrailBasisSpace back to the original RowSpace.
    /// @return The row space obtained by applying each coefficient row to the stored trail basis.
    /// @pre The trail snapshot maps are internally consistent.
    /// @throws std::logic_error if the trail snapshot maps are inconsistent.
    RowSpace to_RowSpace() const;

    /// @brief Determine how many trailing decision levels can be removed while keeping the clause asserting.
    /// @return The number of trailing decision levels that can be removed while the
    /// clause remains asserting.
    /// @pre This trail-basis space has at least one row.
    /// @pre `trail[k]` stores the prefix of learned equations up to decision level `k`.
    /// @pre Every non-root trail snapshot begins with that level's decision equation.
    /// @throws std::runtime_error if this trail-basis space is empty.
    std::size_t jumpable_levels() const;
};
} // namespace algebra
