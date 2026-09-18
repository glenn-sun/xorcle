#pragma once

#include "core/clause_id.h"

namespace xorcle {

class Trail;

/// @brief Why a watched pair of a clause stopped being a legal watch pair.
///
/// A clause with more than one row watches two distinct rows of its linear
/// negation, maintaining, modulo the trail span, that the first is not `0 = 0`
/// and the second is independent of it.
enum class WatchType {
    zero, ///< The first watch reduced to `0 = 0`.
    one, ///< The second watch reduced to `0 = 0`.
    sum ///< The two watches became equal. Normalized to a `one` trigger.
};

/// @brief Receives the watch movements a propagation step performs.
class WatchUpdateHandler {
public:
    virtual ~WatchUpdateHandler() = default;

    /// @brief Report that a watch of clause `id` moved, called *after*
    /// `watched0` / `watched1` are updated.
    virtual void update_with_new_watch(ClauseId id, ClauseRowIdx old_watch, WatchType type,
                                       const Trail& trail) = 0;
};

class IgnoreWatchUpdates final : public WatchUpdateHandler {
public:
    void update_with_new_watch(ClauseId, ClauseRowIdx, WatchType, const Trail&) override {}
};

} // namespace xorcle
