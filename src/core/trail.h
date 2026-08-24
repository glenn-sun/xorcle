#pragma once

#include <cassert>
#include <cstddef>
#include <optional>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"

namespace xorcle {

/// @brief Every equation learned up to and including one decision level, plus
/// its span and the bookkeeping that relates the two.
///
/// `span()` keeps its rows in row-echelon order, unrelated to the order
/// equations were learned in. `order_map()` maps trail insertion order to the
/// row index in `span()`, `reverse_order_map()` is its inverse permutation, and
/// `reasons()[t]` is the reason for the equation learned at trail order `t`, or
/// empty when it was a decision. All three have length `span().rows()`.
class TrailSnapshot {
public:
    TrailSnapshot(EquationRepr repr, std::size_t num_vars) : span_(repr, num_vars) {}

    /// @brief Learn a new equation, with `reason` empty for a decision.
    /// @pre `eqn.num_bits() == span().cols()`, `eqn.repr() == span().repr()`, and
    /// `eqn` is reduced by `span()` so it is not already in the span.
    /// @pre If `reason` is provided, `eqn + [0 = 1]` belongs to it. Not asserted.
    void learn(const Equation& eqn, std::optional<ClauseId> reason);

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

/// @brief The solver trail, stored as one immutable snapshot per decision level.
///
/// `back()` is the live state; `operator[](k)` is the state at the end of
/// decision level `k`. Every non-root snapshot begins, in trail order, with that
/// level's decision equation, and `size() == level() + 1`.
///
/// @note Each snapshot is a full independent copy.
class Trail {
public:
    /// @brief A trail holding a single empty root snapshot.
    Trail(EquationRepr repr, std::size_t num_vars);

    /// @brief Learn a new equation, opening a new decision level when `reason` is
    /// empty.
    /// @pre The same as `TrailSnapshot::learn()`.
    void learn(const Equation& eqn, std::optional<ClauseId> reason);

    std::size_t size() const noexcept { return snapshots_.size(); }

    std::size_t level() const noexcept { return snapshots_.size() - 1; }

    /// @brief The snapshot at the end of decision level `index`.
    /// @pre `index < size()`.
    const TrailSnapshot& operator[](std::size_t index) const {
        assert(index < snapshots_.size() && "decision level is out of range");
        return snapshots_[index];
    }

    const TrailSnapshot& back() const noexcept { return snapshots_.back(); }

    /// @brief Discard trailing decision levels, keeping `num_levels` snapshots.
    ///
    /// Backjumping is `resize(asserting_level + 1)`; restarting is `resize(1)`.
    /// @pre `1 <= num_levels <= size()`.
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
