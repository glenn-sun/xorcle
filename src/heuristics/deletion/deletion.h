#pragma once

/// @file deletion.h
/// @brief Deleting the learned clauses a deletion pass judges least useful.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/clause_id.h"
#include "core/trail.h"
#include "heuristics/decisions/clause_activity.h"
#include "heuristics/selection/clause_selector.h"

namespace xorcle {

/// @brief Which score orders the clauses a deletion pass evicts.
enum class DeletionKey : std::uint8_t {
    lbd,     ///< Higher stored LBD goes first.
    activity ///< Lower clause activity goes first.
};

/// @brief When the solver deletes learned clauses, and by which key.
struct DeletionConfig {
    bool enabled = false;

    std::size_t conflict_interval = 100; ///< Conflicts between deletion passes.

    /// @brief Conflicts a `tier2` clause goes unused before it becomes `local`.
    std::size_t demote_interval = 200;

    ClauseTierConfig tiers;
    DeletionKey key = DeletionKey::activity;

    /// @brief Where `DeletionKey::activity` reads its score; not owned.
    const ClauseActivitySource* activity = nullptr;
};

/// @brief A read-only view of the state a deletion pass is planned against.
struct DeletionState {
    const ClauseSelector& selector;
    const Trail& trail;

    /// @brief Conflicts this pass adds to an unused clause's count, and the count
    /// at which it is demoted.
    std::size_t conflict_interval;
    std::size_t demote_interval;

    DeletionKey key;
    const ClauseActivitySource* activity;
};

/// @brief What one deletion pass changes, in clause-id order.
struct DeletionPlan {
    std::vector<ClauseId> evicted; ///< To tombstone, ascending.
    std::vector<ClauseId> demoted; ///< `tier2` clauses becoming `local`, ascending.

    /// @brief The unused-conflict count after the pass, by clause id.
    std::vector<std::size_t> unused_conflicts;
};

/// @brief Choose the clauses this deletion pass demotes, and the worse half of the
/// `local` tier to evict.
///
/// A clause some trail row names as its reason is locked: neither evicted nor
/// counted as unused. A clause demoted here is not also an eviction candidate.
///
/// @pre `state.activity` is non-null if `state.key` is `DeletionKey::activity`.
DeletionPlan build_deletion_plan(const DeletionState& state);

} // namespace xorcle
