#pragma once

#include "core/clause_id.h"

namespace xorcle {

class Trail;

/// @brief Why a watched pair of a clause stopped being a legal watch pair.
///
/// A clause with more than one row watches two distinct rows @f$g_{w_0}@f$ and
/// @f$g_{w_1}@f$ of its linear negation, maintaining, modulo
/// @f$\mathrm{span}(U)@f$, that @f$g_{w_0} \neq \mathbf{0}@f$ and
/// @f$g_{w_1} \notin \langle g_{w_0} \rangle@f$. The second condition is why
/// `sum` exists: the pair can go bad because the two watches become equal even
/// though neither reduces to @f$\mathbf{0}@f$ alone.
enum class WatchType {
    zero, ///< The first watch reduced to @f$\mathbf{0}@f$.
    one, ///< The second watch reduced to @f$\mathbf{0}@f$.
    sum ///< The two watches became equal. Normalized to a `one` trigger.
};

/// @brief Receives the watch movements a propagation step performs: anything
/// that indexes clauses by their watched rows -- the leading-term watchlist, in
/// particular -- has to be told when a watch moves. Its own header so a selector
/// can implement it without including the propagation engine.
class WatchObserver {
public:
    virtual ~WatchObserver() = default;

    /// @brief Report that a watch of clause `id` moved. Called *after*
    /// `watched0` / `watched1` are updated, so an implementation reads the clause
    /// for the new row and uses `old_watch` for the old one.
    virtual void update_with_new_watch(ClauseId id, EquationId old_watch, WatchType type,
                                       const Trail& trail) = 0;
};

/// @brief A `WatchObserver` that ignores every watch movement: the default for
/// selectors that do not index clauses by watched row, and for tests.
class IgnoreWatchUpdates final : public WatchObserver {
public:
    void update_with_new_watch(ClauseId, EquationId, WatchType, const Trail&) override {}
};

} // namespace xorcle
