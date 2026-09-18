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

/// @brief Which score orders the clauses a deletion pass deletes.
enum class DeletionKey : std::uint8_t {
    lbd,     ///< Higher stored LBD goes first.
    activity ///< Lower clause activity goes first.
};

/// @brief When the solver deletes learned clauses, and by which key.
struct DeletionConfig {
    bool enabled = false;

    std::size_t cadence = 100; ///< Conflicts between deletion passes.

    /// @brief Consecutive deletion passes a `secondary` clause goes unused before it
    /// becomes `local`.
    std::size_t demote_cadence_in_passes = 2;

    ClauseTierConfig tiers;
    DeletionKey key = DeletionKey::activity;

    /// @brief Where `DeletionKey::activity` reads its score; not owned.
    const ClauseActivitySource* activity = nullptr;
};

/// @brief A read-only view of the state a deletion pass is planned against.
struct DeletionState {
    const ClauseSelector& selector;
    const Trail& trail;

    /// @brief The unused-pass count, this pass included, at which a clause is demoted.
    std::size_t demote_cadence_in_passes;

    DeletionKey key;
    const ClauseActivitySource* activity;
};

/// @brief What one deletion pass changes, in clause-id order.
struct DeletionPlan {
    std::vector<ClauseId> deleted; ///< To delete, ascending.
    std::vector<ClauseId> demoted; ///< `secondary` clauses becoming `local`, ascending.

    /// @brief The unused-pass count after the pass, by clause id.
    std::vector<std::size_t> unused_passes;
};

/// @brief Choose the clauses this deletion pass demotes, and the worse half of the
/// `local` tier to delete.
///
/// A clause some trail row names as its reason is locked: neither deleted nor
/// counted as unused. A clause demoted here is not also an deletion candidate.
///
/// @pre `state.activity` is non-null if `state.key` is `DeletionKey::activity`.
DeletionPlan build_deletion_plan(const DeletionState& state);

} // namespace xorcle
