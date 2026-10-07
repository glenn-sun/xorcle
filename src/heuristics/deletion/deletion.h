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
    lbd,     ///< Higher LBD goes first.
    activity ///< Lower clause activity goes first.
};

/// @brief When the solver deletes learned clauses, and by which key.
struct DeletionConfig {
    bool enabled = true;

    /// @brief The conflicts between deletion passes are this times the square root of the
    /// conflicts so far plus `cadence_offset`.
    double cadence_coefficient = 0.25;

    std::size_t cadence_offset = 5000;

    /// @brief The LBD allowed one unused deletion pass; see `allowed_unused_passes()`.
    double wait = 6.0;

    /// @brief The window of the average of the LBDs measured at a clause's uses as a reason;
    /// the newest measurement weighs its reciprocal.
    std::size_t lbd_window = 2;

    /// @brief The fraction of the deletion candidates a deletion pass evicts.
    double eviction_rate = 0.5;

    DeletionKey key = DeletionKey::activity;

    bool operator==(const DeletionConfig&) const = default;
};

/// @brief The consecutive deletion passes a learned clause of this LBD may go unused,
/// `round(2^(wait - lbd))`.
std::size_t allowed_unused_passes(double lbd, double wait);

/// @brief Whether a deletion pass may delete this clause once no trail row names it.
/// @pre `selector.is_clause_learned(id)`.
bool is_deletion_candidate(const ClauseSelector& selector, ClauseId id, double wait);

/// @brief Average the LBD measured at a use as a reason into a learned clause's LBD, and start
/// its unused passes over if it was a deletion candidate.
void update_with_reason_use(ClauseSelector& selector, ClauseId id, std::size_t measured_lbd,
                            const DeletionConfig& config);

/// @brief The conflicts between the last deletion pass and the next, at least one.
std::size_t conflicts_between_deletion_passes(const DeletionConfig& config,
                                              std::size_t num_conflicts);

/// @brief A read-only view of the state a deletion pass is planned against.
struct DeletionState {
    const ClauseSelector& selector;
    const Trail& trail;
    double wait;
    double eviction_rate;
    DeletionKey key;
    const ClauseActivitySource* activity;
};

/// @brief What one deletion pass changes, in clause-id order.
struct DeletionPlan {
    std::vector<ClauseId> deleted; ///< To delete, ascending.

    /// @brief Clauses reaching their allowed unused passes at this pass, ascending.
    std::vector<ClauseId> out_of_unused_passes;

    /// @brief The unused-pass count after the pass, by clause id; zero for an
    /// inactive clause.
    std::vector<std::size_t> unused_passes;
};

/// @brief Choose the clauses running out of unused passes at this deletion pass, and the
/// deletion candidates to evict, worst first.
///
/// A clause some trail row names as its reason is locked: neither deleted nor
/// counted as unused. A clause running out here is not also a deletion candidate here.
///
/// @pre `state.activity` is non-null if `state.key` is `DeletionKey::activity`, and
/// `state.eviction_rate` is in `[0, 1]`.
DeletionPlan build_deletion_plan(const DeletionState& state);

} // namespace xorcle
