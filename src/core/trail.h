#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "algebra/algebra.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"

namespace xorcle {

/// @brief A snapshot of every equation learned up to and including one decision
/// level, together with the space they span.
///
/// This is the paper's @f$U@f$ restricted to levels @f$\le k@f$, plus
/// @f$\mathrm{span}(U)@f$ and the bookkeeping that relates the two.
///
/// Representation invariants:
/// - `span()`, `order_map()`, `reverse_order_map()` and `reasons()` all describe
///   the same learned rows, so all three vectors have length `span().rows()`;
/// - `order_map()` maps trail insertion order to the row index in `span()`;
/// - `reverse_order_map()` is the inverse permutation of `order_map()`;
/// - `reasons()[t]` is the reason for the equation learned at trail order `t`,
///   or `std::nullopt` when that equation was a decision.
///
/// @note The two maps are needed because `span()` keeps its rows in row-echelon
/// order, which is unrelated to the order in which equations were learned.
/// Because `add_row_get_index()` reduces an incoming equation by the rows
/// already present, the row created at trail step @f$t@f$ is @f$e_t@f$ plus a
/// combination of @f$e_0, \dots, e_{t-1}@f$, so the change of basis between the
/// raw trail equations and the stored rows is unitriangular in trail order.
/// That is what makes trail-order arguments valid over row-echelon storage.
class TrailSnapshot {
public:
    /// @brief Construct an empty snapshot.
    /// @param backend Storage representation to use.
    /// @param num_vars Number of variable columns. The constant column is added internally.
    TrailSnapshot(Algebra backend, std::size_t num_vars) : span_(backend, num_vars) {}

    /// @brief Learn a new equation into this snapshot.
    /// @param eqn The equation to learn.
    /// @param reason The reason clause for `eqn`, or `std::nullopt` if `eqn` is a decision.
    /// @pre `eqn.size() == span().cols()`.
    /// @pre `eqn` has been reduced by `span()` before learning, so it is not
    /// already in the span.
    /// @pre If `reason` is provided, then `eqn + `@f$\mathbf{1}@f$ belongs to that
    /// reason clause.
    void learn(const Equation& eqn, std::optional<ClauseId> reason);

    /// @brief The space of equations known to be true, i.e. @f$\mathrm{span}(U)@f$.
    const RowSpace& span() const noexcept { return span_; }

    /// @brief The map from trail insertion order to the row index in `span()`.
    const std::vector<std::size_t>& order_map() const noexcept { return order_map_; }

    /// @brief The map from the row index in `span()` to trail insertion order.
    const std::vector<std::size_t>& reverse_order_map() const noexcept {
        return reverse_order_map_;
    }

    /// @brief The map from trail insertion order to reason clause.
    const std::vector<std::optional<ClauseId>>& reasons() const noexcept { return reasons_; }

private:
    std::vector<std::size_t> order_map_;
    std::vector<std::size_t> reverse_order_map_;
    RowSpace span_;
    std::vector<std::optional<ClauseId>> reasons_;
};

/// @brief The solver trail: the paper's @f$U@f$, stored as one immutable
/// snapshot per decision level.
///
/// `back()` is the live state. `operator[](k)` is the state as of the end of
/// decision level `k`, and holds every equation learned at levels @f$\le k@f$.
/// Every non-root snapshot begins, in trail order, with that level's decision
/// equation, and `size() == level() + 1`.
///
/// A decision copies the whole current snapshot and pushes it; a propagation
/// mutates the current snapshot in place. Backjumping is `resize()`, with no
/// undo logic at all.
///
/// @note The full copy per decision is deliberate and must not be replaced by a
/// copy-on-write or delta scheme. Computing the asserting level walks the
/// snapshot stack reading `operator[](k).span().rows()` for each level, which
/// requires every level to hold an independent matrix.
///
/// @note This deliberately does not derive from `std::vector<TrailSnapshot>`,
/// which the old tree did. The accessors below
/// cover every use the old call sites made of the inherited container interface.
class Trail {
public:
    /// @brief Construct a trail holding a single empty root snapshot.
    /// @param backend Storage representation to use.
    /// @param num_vars Number of variable columns. The constant column is added internally.
    Trail(Algebra backend, std::size_t num_vars);

    /// @brief Learn a new equation, opening a new decision level when `reason` is empty.
    /// @param eqn The equation to learn.
    /// @param reason The reason clause for `eqn`, or `std::nullopt` if `eqn` is a decision.
    /// @pre `eqn.size() == span().cols()`.
    /// @pre `eqn` has been reduced by `span()` before learning, so it is not
    /// already in the span.
    /// @pre If `reason` is provided, then `eqn + `@f$\mathbf{1}@f$ belongs to that
    /// reason clause.
    void learn(const Equation& eqn, std::optional<ClauseId> reason);

    /// @brief Number of snapshots, i.e. `level() + 1`.
    std::size_t size() const noexcept { return snapshots_.size(); }

    /// @brief The current decision level, i.e. `size() - 1`.
    std::size_t level() const noexcept { return snapshots_.size() - 1; }

    /// @brief The snapshot as of the end of decision level `index`.
    /// @param index A decision level.
    /// @pre `index < size()`.
    const TrailSnapshot& operator[](std::size_t index) const { return snapshots_[index]; }

    /// @brief The live snapshot, i.e. the one for the current decision level.
    const TrailSnapshot& back() const noexcept { return snapshots_.back(); }

    /// @brief Discard trailing decision levels, keeping `num_levels` snapshots.
    ///
    /// This is the backjump. Backjumping to the asserting level is
    /// `resize(asserting_level + 1)`; restarting is `resize(1)`.
    /// @param num_levels Number of snapshots to keep, i.e. the new `size()`.
    /// @pre `1 <= num_levels <= size()`. The root snapshot is never discarded,
    /// and this only ever truncates.
    void resize(std::size_t num_levels);

    /// @brief Which backend stores the trail's equations.
    Algebra backend() const noexcept { return span().backend(); }

    /// @brief Number of variable columns.
    std::size_t num_vars() const noexcept { return span().num_vars(); }

    /// @brief The space of equations known to be true, i.e. @f$\mathrm{span}(U)@f$.
    const RowSpace& span() const noexcept { return snapshots_.back().span(); }

    /// @brief The map from trail insertion order to the row index in `span()`.
    const std::vector<std::size_t>& order_map() const noexcept {
        return snapshots_.back().order_map();
    }

    /// @brief The map from the row index in `span()` to trail insertion order.
    const std::vector<std::size_t>& reverse_order_map() const noexcept {
        return snapshots_.back().reverse_order_map();
    }

    /// @brief The map from trail insertion order to reason clause.
    const std::vector<std::optional<ClauseId>>& reasons() const noexcept {
        return snapshots_.back().reasons();
    }

private:
    std::vector<TrailSnapshot> snapshots_;
};

} // namespace xorcle
