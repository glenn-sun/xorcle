#pragma once

#include <cstddef>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/trail.h"

namespace xorcle {

/// @brief A subspace of the trail span, written in the
/// @f$\mathbf{1}, f_m, \dots, f_1@f$ basis.
///
/// Each *column* is a coordinate against one row of `trail.span()`, in **reverse
/// trail order**: column 0 is the most recently learned equation and column
/// @f$m - 1@f$ the oldest, where @f$m = \texttt{trail.span().rows()}@f$. Column
/// `c` is the coordinate of `trail.span().row(trail.order_map()[m - 1 - c])` --
/// the row-echelon row trail step @f$m - 1 - c@f$ created, *not* the raw equation
/// handed to `Trail::learn()`. The two differ by a change of basis that is
/// unitriangular in trail order (see `TrailSnapshot`), which is why trail-order
/// arguments -- above all `asserting_level()` -- stay valid over row-echelon
/// storage.
///
/// Reverse order is what makes the basis useful: `add_row()` echelonizes, so
/// rows come out in ascending leading-column order, which is *descending* trail
/// order. Row 0 therefore involves the most recently learned equation, and
/// `pop_first_row()` peels off exactly the row conflict analysis resolves next.
/// Rows are coefficient vectors over trail rows, so their width is @f$m@f$ and
/// has nothing to do with the number of variables.
///
/// @warning The inherited "constant column" is **not a constant here.** A
/// `RowEchelonMatrix` over `num_vars` variables reserves its last column for the
/// right-hand side; this class passes `num_vars == m - 1` purely to obtain
/// `cols() == m`, so that last column is an ordinary coordinate -- the *oldest*
/// trail row's. Consequently `ReduceResult::Kind::eq_false` is meaningless here:
/// a row whose only set column is `m - 1` is not @f$\mathbf{1}@f$, it is the
/// oldest trail row's coordinate vector. Never call `classify()`,
/// `is_eq_false()`, `partial_reduce()`, `partial_reduce_with_extra()` or
/// `solution()` on a `TrailBasisSpace`; it is meant for `add_row()`,
/// `pop_first_row()`, `row()`, `rows()`, `cols()`, iteration and
/// `asserting_level()`.
///
/// @note The trail reference is non-owning, and `to_RowSpace()` and
/// `asserting_level()` re-read the *live* trail on every call, so learning or
/// backjumping invalidates an already-built `TrailBasisSpace`.
class TrailBasisSpace : public RowEchelonMatrix {
public:
    explicit TrailBasisSpace(const Trail& trail)
        : RowEchelonMatrix(trail.repr(),
                           trail.span().rows() > 0 ? trail.span().rows() - 1 : 0),
          trail_(trail) {}

    /// @brief Rewrite a row space into trail-basis coordinates, echelonized.
    /// @pre `trail.span()` is non-empty, its order maps are consistent, and
    /// every row of `rs` belongs to it.
    static TrailBasisSpace from_RowSpace(const RowSpace& rs, const Trail& trail);

    /// @brief Convert back out of trail-basis coordinates. Round-trips through
    /// `from_RowSpace()` preserve the *span*, not the individual rows: both
    /// directions echelonize what they build.
    RowSpace to_RowSpace() const;

    /// @brief The decision level this clause asserts at: the lowest level `a`
    /// such that the clause is still asserting once every level above `a` is
    /// discarded.
    ///
    /// The result is in `[0, trail.level()]`; backjumping is
    /// `trail.resize(asserting_level() + 1)`. Both boundary values are easy to
    /// invert: `== trail.level()` means **nothing** can be discarded, the clause
    /// still depends on the current level (conflict analysis loops while that
    /// holds -- *not* while `== 0`); `== 0` means every non-root level can go,
    /// and a single-row basis is a lifted unit clause and always returns 0.
    ///
    /// Unfolding the coordinate mapping gives a characterization easier to check
    /// by hand than the level walk the implementation does: with `c` the leading
    /// column of row 1, the answer is **the decision level at which the trail
    /// equation of step @f$m - 1 - c@f$ was learned**. Row 0 is the row about to
    /// be resolved away and is skipped; every row after row 1 has a larger
    /// leading column, hence an older trail step and a lower level, so row 1 is
    /// the deepest reach of the surviving basis.
    ///
    /// @pre `rows() > 0`.
    /// @pre `trail[k]` holds every equation learned at levels @f$\le k@f$, and
    /// every non-root snapshot begins in trail order with that level's decision.
    /// @note @f$O(\texttt{trail.level()})@f$, called twice per resolution step.
    std::size_t asserting_level() const;

private:
    const Trail& trail_; ///< The trail whose rows index this matrix's columns.
};

} // namespace xorcle
