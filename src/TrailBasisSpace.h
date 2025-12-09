#pragma once

#include "Trail.h"

/// @brief A vector of coefficients over GF(2).
/// 
/// Used when representing equations as linear combinations of other
/// equations. The length of this vector is the dimension of the current
/// subspace, not the number of variables + 1. 
class Coefficients : public bit::vector<> {
public:
    Coefficients() {}
    Coefficients(const bit::vector<>& v) : bit::vector<>(v) {}
};



/// @brief A subspace of a trail span, represented as a basis in 
/// row-echelon form. Each column corresponds to an equation in the trail,
/// in the reverse trail appending order (i.e. the first column corresponds
/// to the last equation in the trail, i.e. often false).
class TrailBasisSpace : public REFMatrix {
private:
    const Trail& trail;

public:
    TrailBasisSpace(const Trail& trail) : REFMatrix(), trail(trail) {}

    /// @brief Construct a TrailBasisSpace from a RowSpace w.r.t. a Trail
    ///
    /// In particular, if the trail contains m linearly independent equations, 
    /// and the row space contains k equations, then this will have k equations 
    /// in m variables, where the i-th "variable" is the coefficient of the 
    /// trail.order_map[m-i-1]-th row in trail.span().
    ///
    /// For example, if the trail is [[x1 + x2 = 1], [x3 = 0]] with [x3 = 0] inserted
    /// before [x1 + x2 = 1], and the row space is simply [x3 = 0], then 
    /// in the trail basis this is [0, 1].
    ///
    /// Requires: rs belongs to trail.span()
    static TrailBasisSpace from_RowSpace(const RowSpace& rs, const Trail& trail);

    /// @brief Convert this TrailBasisSpace back to the original RowSpace
    RowSpace to_RowSpace() const;

    /// @brief Determine the maximum number of trail snapshots for which the
    /// second row starts is zero. The clause is asserting if jumpable_level() > 0.
    std::size_t jumpable_levels() const;
};