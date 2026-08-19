#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"

namespace xorcle {

/// @brief Every equation learned up to and including one decision level, i.e.
/// @f$U@f$ restricted to levels @f$\le k@f$, plus @f$\mathrm{span}(U)@f$ and the
/// bookkeeping that relates the two.
///
/// Representation invariants: `span()`, `order_map()`, `reverse_order_map()` and
/// `reasons()` all describe the same learned rows, so all three vectors have
/// length `span().rows()`; `order_map()` maps trail insertion order to the row
/// index in `span()` and `reverse_order_map()` is its inverse permutation;
/// `reasons()[t]` is the reason for the equation learned at trail order `t`, or
/// `std::nullopt` when that equation was a decision.
///
/// @note The two maps are needed because `span()` keeps its rows in row-echelon
/// order, unrelated to the order equations were learned in. Because
/// `add_row_get_index()` reduces an incoming equation by the rows already
/// present, the row created at trail step @f$t@f$ is @f$e_t@f$ plus a
/// combination of @f$e_0, \dots, e_{t-1}@f$, so the change of basis between the
/// raw trail equations and the stored rows is unitriangular in trail order --
/// which is what makes trail-order arguments valid over the row-echelon
/// representation.
class TrailSnapshot {
public:
    TrailSnapshot(EquationRepr repr, std::size_t num_vars) : span_(repr, num_vars) {}

    /// @brief Learn a new equation, with `reason` empty for a decision.
    /// @pre `eqn.num_bits() == span().cols()`, and `eqn` has been reduced by `span()`
    /// so it is not already in the span.
    /// @pre If `reason` is provided, then `eqn + `@f$\mathbf{1}@f$ belongs to that
    /// reason clause.
    void learn(const Equation& eqn, std::optional<ClauseId> reason);

    /// @brief The equations known to be true, i.e. @f$\mathrm{span}(U)@f$.
    const RowSpace& span() const noexcept { return span_; }

    const std::vector<std::size_t>& order_map() const noexcept { return order_map_; }

    const std::vector<std::size_t>& reverse_order_map() const noexcept {
        return reverse_order_map_;
    }

    const std::vector<std::optional<ClauseId>>& reasons() const noexcept { return reasons_; }

private:
    std::vector<std::size_t> order_map_;
    std::vector<std::size_t> reverse_order_map_;
    RowSpace span_;
    std::vector<std::optional<ClauseId>> reasons_;
};

/// @brief The solver trail @f$U@f$, stored as one immutable snapshot per
/// decision level.
///
/// `back()` is the live state; `operator[](k)` is the state at the end of
/// decision level `k`, holding every equation learned at levels @f$\le k@f$.
/// Every non-root snapshot begins, in trail order, with that level's decision
/// equation, and `size() == level() + 1`. A decision copies the whole current
/// snapshot and pushes it; a propagation mutates it in place. Backjumping is
/// `resize()`, with no undo logic at all.
///
/// @note The full copy per decision is deliberate and must not be replaced by a
/// copy-on-write or delta scheme. Computing the asserting level walks the
/// snapshot stack reading `operator[](k).span().rows()` for each level, which
/// requires every level to hold an independent matrix.
class Trail {
public:
    /// @brief A trail holding a single empty root snapshot.
    Trail(EquationRepr repr, std::size_t num_vars);

    /// @brief Learn a new equation, opening a new decision level when `reason` is
    /// empty. Same preconditions as `TrailSnapshot::learn()`.
    void learn(const Equation& eqn, std::optional<ClauseId> reason);

    std::size_t size() const noexcept { return snapshots_.size(); }

    std::size_t level() const noexcept { return snapshots_.size() - 1; }

    /// @brief The snapshot at the end of decision level `index`.
    /// @pre `index < size()`.
    const TrailSnapshot& operator[](std::size_t index) const { return snapshots_[index]; }

    const TrailSnapshot& back() const noexcept { return snapshots_.back(); }

    /// @brief Discard trailing decision levels, keeping `num_levels` snapshots.
    /// This is the backjump: backjumping to the asserting level is
    /// `resize(asserting_level + 1)`, and restarting is `resize(1)`.
    /// @pre `1 <= num_levels <= size()`. The root snapshot is never discarded,
    /// and this only ever truncates.
    void resize(std::size_t num_levels);

    EquationRepr repr() const noexcept { return span().repr(); }

    std::size_t num_vars() const noexcept { return span().num_vars(); }

    /// @brief The live snapshot's accessors, forwarded.
    const RowSpace& span() const noexcept { return snapshots_.back().span(); }

    const std::vector<std::size_t>& order_map() const noexcept {
        return snapshots_.back().order_map();
    }

    const std::vector<std::size_t>& reverse_order_map() const noexcept {
        return snapshots_.back().reverse_order_map();
    }

    const std::vector<std::optional<ClauseId>>& reasons() const noexcept {
        return snapshots_.back().reasons();
    }

private:
    std::vector<TrailSnapshot> snapshots_;
};

} // namespace xorcle
