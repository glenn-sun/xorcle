#pragma once

#include <cassert>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"

namespace xorcle {

/// @brief Rows written as sums of trail rows, each the trail indices of its
/// summands.
struct TrailCoordinates {
    std::size_t rows() const noexcept { return row_ends.size(); }

    std::vector<std::size_t> trail_idxs;
    std::vector<std::size_t> row_ends; ///< Where each row's indices end in `trail_idxs`.
};

/// @brief The solver trail: every equation assumed or derived, ordered by
/// `(level, derivation order within the level)`, plus its span and the
/// bookkeeping that relates the two.
///
/// `span()` keeps its rows in row-echelon order, unrelated to trail order.
/// `trail_idx_to_row_idx()` maps trail order to the row index in `span()`,
/// `row_idx_to_trail_idx()` is its inverse permutation, and `reasons()[t]` is the
/// reason for the equation at trail order `t`, or empty when it was a decision.
/// All three have length `span().rows()`.
///
/// The span's rows are echelonized in trail order, so every stored row lies in
/// the span of the equations at or below its own equation's level.
class Trail {
public:
    /// @brief A trail holding nothing, at the root level.
    Trail(EquationRepr repr, std::size_t num_vars);

    /// @brief Learn a new equation at `level`, with `reason` empty for a decision.
    /// @return The trail order index the equation took.
    /// @pre `eqn.num_bits() == span().cols()` and `eqn.repr() == span().repr()`.
    /// @pre `eqn` is not in the span of the equations at `level` or below.
    /// @pre `level == level() + 1` for a decision, which opens that level, and
    /// `level <= level()` otherwise.
    /// @pre If `reason` is provided, `eqn + [0 = 1]` belongs to it. Not asserted.
    std::size_t learn(const Equation& eqn, std::optional<ClauseId> reason, std::size_t level);

    std::size_t level() const noexcept { return level_end_.size() - 1; }

    /// @brief How many equations the trail holds at `level` or below, which are
    /// the first that many in trail order.
    /// @pre `level <= level()`.
    std::size_t rows_at_level(std::size_t level) const {
        assert(level < level_end_.size() && "decision level is out of range");
        return level_end_[level];
    }

    /// @brief The decision level of the equation at trail order `trail_idx`.
    /// @pre `trail_idx < span().rows()`.
    std::size_t level_of(std::size_t trail_idx) const {
        assert(trail_idx < levels_.size() && "trail order index is out of range");
        return levels_[trail_idx];
    }

    /// @brief Discard every decision level above `num_levels - 1`.
    ///
    /// Backjumping is `resize(asserting_level + 1)`; restarting is `resize(1)`.
    /// @pre `1 <= num_levels <= level() + 1`.
    void resize(std::size_t num_levels);

    EquationRepr repr() const noexcept { return span_.repr(); }

    std::size_t num_vars() const noexcept { return span_.num_vars(); }

    const RowSpace& span() const noexcept { return span_; }

    const std::vector<std::size_t>& trail_idx_to_row_idx() const {
        if (trail_idx_to_row_idx_is_stale_) {
            rebuild_trail_idx_to_row_idx();
        }
        return trail_idx_to_row_idx_;
    }

    /// @brief The stored row of the equation at trail order `trail_idx`.
    /// @pre `trail_idx < span().rows()`.
    const Equation& row(std::size_t trail_idx) const {
        assert(trail_idx < levels_.size() && "trail order index is out of range");
        if (trail_idx_to_row_idx_is_stale_ && trail_idx + 1 == levels_.size()) {
            return span_.row(last_row_idx_);
        }
        return span_.row(trail_idx_to_row_idx()[trail_idx]);
    }

    const std::vector<std::size_t>& row_idx_to_trail_idx() const noexcept {
        return row_idx_to_trail_idx_;
    }

    const std::vector<std::optional<ClauseId>>& reasons() const noexcept { return reasons_; }

    /// @brief The coordinates stored for the reason of the row at `trail_idx`,
    /// or null; they stay until a row at or below `trail_idx` changes.
    /// @pre `trail_idx < span().rows()`.
    const TrailCoordinates* reason_coordinates(std::size_t trail_idx) const {
        assert(trail_idx < reason_coordinates_.size() && "trail order index is out of range");
        const std::optional<TrailCoordinates>& coordinates = reason_coordinates_[trail_idx];
        return coordinates.has_value() ? &*coordinates : nullptr;
    }

    /// @pre `trail_idx < span().rows()`, and `coordinates` are the reason's.
    void store_reason_coordinates(std::size_t trail_idx, TrailCoordinates coordinates) const {
        assert(trail_idx < reason_coordinates_.size() && "trail order index is out of range");
        reason_coordinates_[trail_idx] = std::move(coordinates);
    }

private:
    /// @brief Learn at the level the trail is already at, where the equation's
    /// row goes straight in.
    std::size_t learn_in_order(const Equation& eqn, std::optional<ClauseId> reason,
                               std::size_t level);

    /// @brief Learn below the current level, rewriting only the rows along the
    /// chain of pivot columns the new row displaces.
    std::size_t learn_out_of_order(const Equation& eqn, std::optional<ClauseId> reason,
                                   std::size_t level, std::size_t trail_idx);

    void rebuild_row_idx_to_trail_idx();

    void rebuild_trail_idx_to_row_idx() const;

#ifndef NDEBUG
    void debug_assert_valid() const;

    /// @brief Check the rows and order maps a chain insert at `trail_idx`
    /// produced against taking every row above it out and putting it back.
    void debug_assert_matches_reinsertion(const Trail& before, const Equation& eqn,
                                          std::size_t trail_idx) const;
#endif

    RowSpace span_;

    /// @brief Rebuilt from `row_idx_to_trail_idx_` on the first read after an in-order learn.
    mutable std::vector<std::size_t> trail_idx_to_row_idx_;
    mutable bool trail_idx_to_row_idx_is_stale_ = false;

    /// @brief The row index the last in-order learn took.
    std::size_t last_row_idx_ = 0;

    std::vector<std::size_t> row_idx_to_trail_idx_;
    std::vector<std::optional<ClauseId>> reasons_;
    std::vector<std::size_t> levels_;

    /// @brief A cache for conflict analysis, which reads it through a const trail.
    mutable std::vector<std::optional<TrailCoordinates>> reason_coordinates_;

    /// @brief How many equations lie at each level or below, indexed by level.
    std::vector<std::size_t> level_end_;
};

} // namespace xorcle
