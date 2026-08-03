#pragma once

#include "core/clause_id.h"

namespace xorcle {

class Trail;

/// @brief Why a watched pair of a clause stopped being a legal watch pair.
///
/// A clause with more than one row watches two distinct rows @f$g_{w_0}@f$ and
/// @f$g_{w_1}@f$ of its linear negation, maintaining, modulo
/// @f$\mathrm{span}(U)@f$:
/// - @f$g_{w_0} \neq \mathbf{0}@f$, and
/// - @f$g_{w_1} \notin \langle g_{w_0} \rangle@f$, i.e. neither
///   @f$g_{w_1} = \mathbf{0}@f$ nor @f$g_{w_1} = g_{w_0}@f$.
///
/// The second half is why `sum` exists: the pair can go bad because the two
/// watches become equal even though neither reduces to @f$\mathbf{0}@f$ alone.
enum class WatchType {
    zero, ///< The first watch reduced to @f$\mathbf{0}@f$.
    one, ///< The second watch reduced to @f$\mathbf{0}@f$.
    sum ///< The two watches became equal. Normalized to a `one` trigger.
};

/// @brief Receives the watch movements a propagation step performs.
///
/// The propagation engine owns the watch invariant and moves watches; anything
/// that indexes clauses by their watched rows -- the leading-term watchlist, in
/// particular -- has to be told when one moves. Implemented by the clause
/// selector, which is a heuristic: `core/` moves
/// the watch, `heuristics/selection/` reacts to it.
///
/// This lives in its own header so a selector can take part in the watch
/// protocol without including the propagation engine.
class WatchObserver {
public:
    virtual ~WatchObserver() = default;

    /// @brief Report that a watch of clause `id` moved.
    /// @param id The clause whose watch moved.
    /// @param old_watch The row the watch pointed at before the move.
    /// @param type Which of the two watches moved.
    /// @param trail The current trail.
    /// @note Called *after* the clause's `watched0` / `watched1` have been
    /// updated, so an implementation reads the clause to find the new row and
    /// uses `old_watch` for the old one.
    virtual void update_with_new_watch(ClauseId id, EquationId old_watch, WatchType type,
                                       const Trail& trail) = 0;
};

/// @brief A `WatchObserver` that ignores every watch movement.
///
/// The default for selectors that do not index clauses by watched row, and for
/// tests.
class IgnoreWatchUpdates final : public WatchObserver {
public:
    /// @brief Do nothing.
    void update_with_new_watch(ClauseId, EquationId, WatchType, const Trail&) override {}
};

} // namespace xorcle
