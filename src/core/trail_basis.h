#pragma once

#include <cstddef>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/trail.h"

namespace xorcle {

/// @brief A subspace of the trail span, written in the paper's
/// @f$U_\text{reverse} = \mathbf{1}, f_m, \dots, f_1@f$ basis.
///
/// Each *column* of this matrix is a coordinate against one row of
/// `trail.span()`, ordered by **reverse trail order**: column 0 is the most
/// recently learned equation and column @f$m - 1@f$ the oldest, where @f$m =
/// \texttt{trail.span().rows()}@f$. Precisely, column `c` is the coordinate of
///
/// @code
/// trail.span().row(trail.order_map()[m - 1 - c])
/// @endcode
///
/// -- the row-echelon row that trail step @f$m - 1 - c@f$ created, *not* the raw
/// equation @f$e_{m-1-c}@f$ the solver handed to `Trail::learn()`. The two
/// differ by a change of basis that is unitriangular in trail order (see
/// `TrailSnapshot`), which is exactly why trail-order arguments -- above all
/// `asserting_level()` -- stay valid over row-echelon storage.
///
/// Reverse order is what makes the basis useful: `add_row()` echelonizes, so
/// rows come out in ascending leading-column order, which is *descending* trail
/// order. Row 0 is therefore the row involving the most recently learned
/// equation, and `pop_first_row()` peels off exactly the row conflict analysis
/// must resolve next.
///
/// @warning The inherited "constant column" is **not a constant here.** A
/// `RowEchelonMatrix` over `num_vars` variables reserves its last column, index
/// `num_vars`, for the right-hand side; this class is built with `num_vars ==
/// m - 1` purely to obtain `cols() == m`, so that last column is an ordinary
/// coordinate -- the one belonging to the *oldest* trail row. Consequently
/// `ReduceResult::Kind::eq_false` is meaningless on rows of this class: a row
/// whose only set column is `m - 1` is not @f$\mathbf{1}@f$, it is the oldest
/// trail row's coordinate vector. Never call `classify()`, `is_eq_false()`,
/// `partial_reduce()`, `partial_reduce_with_extra()` or `solution()` on a
/// `TrailBasisSpace`; the operations it is meant for are `add_row()`,
/// `pop_first_row()`, `row()`, `rows()`, `cols()`, iteration, and
/// `asserting_level()`.
///
/// @note This holds a non-owning reference to the trail, and `to_RowSpace()`
/// and `asserting_level()` re-read the *live* trail on every call. Building a
/// `TrailBasisSpace` and then learning or backjumping invalidates it.
///
/// @note Rows of this matrix are coefficient vectors over trail rows, so their
/// width is @f$m@f$ and has nothing to do with the number of variables. The old
/// tree marked that with a `Coefficients` type deriving from its bit vector;
/// deriving from storage is banned, and an alias of
/// `Equation` would carry no more type safety than `Equation` itself, so the
/// distinction is documented rather than declared. `Bitmap` already carries the
/// other, unrelated meaning of "coefficients" -- the row selector that
/// `RowEchelonMatrix::reduce()` fills in.
class TrailBasisSpace : public RowEchelonMatrix {
public:
    /// @brief Construct an empty trail-basis space over a trail's rows.
    /// @param trail Trail whose learned equations define the coefficient basis.
    /// @note With `m == trail.span().rows()` rows in the span, this matrix takes
    /// coefficient rows of width `m`, i.e. `cols() == m` and `num_vars() == m - 1`.
    /// See the warning above: that `num_vars()` is an artefact of the width
    /// bookkeeping and names nothing.
    explicit TrailBasisSpace(const Trail& trail)
        : RowEchelonMatrix(trail.backend(),
                           trail.span().rows() > 0 ? trail.span().rows() - 1 : 0),
          trail_(trail) {}

    /// @brief Rewrite a row space into trail-basis coordinates.
    /// @param rs Row space to express in trail-basis coordinates.
    /// @param trail Trail whose span supplies the basis.
    /// @return The rows of `rs` as coefficient rows in reverse trail order, echelonized.
    /// @pre `trail.span().rows() != 0`.
    /// @pre `rs.cols() == trail.span().cols()` and `rs.backend() == trail.backend()`.
    /// @pre Every row of `rs` belongs to `trail.span()`.
    /// @pre The trail snapshot's order maps are consistent.
    /// @note All four preconditions are internal invariants of the solver, so
    /// they are asserts rather than exceptions.
    static TrailBasisSpace from_RowSpace(const RowSpace& rs, const Trail& trail);

    /// @brief Convert back out of trail-basis coordinates.
    /// @return The row space spanned by applying each coefficient row to the trail rows.
    /// @pre The trail snapshot's order maps are consistent.
    /// @note Round-trips through `from_RowSpace()` preserve the *span*, not the
    /// individual rows: both directions echelonize what they build.
    RowSpace to_RowSpace() const;

    /// @brief The decision level this clause asserts at, i.e. the level to backjump to.
    ///
    /// The result is in `[0, trail.level()]`. Backjumping is
    /// `trail.resize(asserting_level() + 1)`, and the number of levels discarded
    /// is `trail.level() - asserting_level()`.
    ///
    /// Two boundary values are worth naming, because both are easy to invert:
    /// - `asserting_level() == trail.level()` means **nothing** can be
    ///   discarded; the clause still depends on the current level. Conflict
    ///   analysis loops while this holds:
    ///   `while (basis.asserting_level() == trail.level()) { ... }` -- *not*
    ///   `== 0`.
    /// - `asserting_level() == 0` means every non-root level can be discarded.
    ///   A single-row trail-basis space is a lifted unit clause and always
    ///   returns 0.
    ///
    /// Unfolding the coordinate mapping gives a characterization that is easier
    /// to check by hand than the level walk the implementation does: with `c`
    /// the leading column of row 1, the answer is **the decision level at which
    /// the trail equation of step @f$m - 1 - c@f$ was learned**. Row 0 is the
    /// row about to be resolved away and is skipped; every row after row 1 has a
    /// larger leading column, hence an older trail step and a lower level, so
    /// row 1 is the deepest reach of the surviving basis.
    ///
    /// @return The lowest level `a` such that the clause is still asserting
    /// after every level above `a` is discarded.
    /// @pre `rows() > 0`.
    /// @pre `trail[k]` holds every equation learned at levels @f$\le k@f$, and
    /// every non-root snapshot begins in trail order with that level's decision.
    /// @note This is @f$O(\texttt{trail.level()})@f$ and conflict analysis calls
    /// it twice per resolution step -- once in the loop condition and once
    /// after. That is cheap enough to leave alone, but it is not free.
    ///
    /// @note **This was `jumpable_levels()` in the 0.1 tree, and the sign is
    /// flipped:** that method returned the *count of levels to discard*, so
    /// `asserting_level() == trail.level() - jumpable_levels()`. Every
    /// comparison against it has to be turned inside out; see the two boundary
    /// values above.
    std::size_t asserting_level() const;

private:
    const Trail& trail_; ///< The trail whose rows index this matrix's columns.
};

} // namespace xorcle
