#pragma once

/// @file deletion.h
/// @brief Deleting the learned clauses a deletion pass judges least useful.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/clause_id.h"
#include "core/trail.h"
#include "heuristics/decisions/csids.h"
#include "core/clause_database.h"

namespace xorcle {

/// @brief Which score orders the clauses a deletion pass deletes.
enum class DeletionKey : std::uint8_t {
    lbd,     ///< Higher LBD goes first.
    activity ///< Lower clause activity goes first.
};

/// @brief When the solver deletes learned clauses, and by which key.
struct DeletionConfig {
    /// @brief The conflicts between deletion passes are this times the square root of the
    /// conflicts so far plus `cadence_offset`.
    double cadence_coefficient;

    std::size_t cadence_offset;

    /// @brief The LBD allowed one unused deletion pass; see `free_passes()`.
    double wait;

    /// @brief The fraction of the deletion candidates a deletion pass evicts.
    double eviction_rate;

    DeletionKey key;
};

/// @brief The free passes of a learned clause of this LBD, `round(2^(wait - lbd))`: the
/// consecutive deletion passes it may go unused.
std::size_t free_passes(double lbd, double wait);

/// @brief Mark a learned clause used as a reason since the last deletion pass; a starting
/// clause's record is left alone.
void update_with_reason_use(ClauseRecord& record);

/// @brief The conflicts between the last deletion pass and the next, at least one.
std::size_t conflicts_between_deletion_passes(const DeletionConfig& config,
                                              std::size_t num_conflicts);

/// @brief A read-only view of the state a deletion pass is planned against.
struct DeletionState {
    const ClauseDatabase& database;
    const Trail& trail;
    const DeletionConfig& config;
    const Csids& activity;
};

/// @brief What one deletion pass changes, in clause-id order.
struct DeletionPlan {
    std::vector<ClauseId> deleted; ///< To delete, ascending.

    /// @brief The unused-pass count after the pass of each active clause, in the
    /// order of `ClauseDatabase::active_clause_ids()`.
    std::vector<std::size_t> unused_passes;
};

/// @brief Count each learned clause's unused passes and choose the deletion candidates to
/// evict, worst first.
///
/// A clause used since the last pass starts its unused passes over; otherwise a clause
/// unused for as many passes as `free_passes()` gives it is a deletion
/// candidate, and any other counts one more. A clause some trail row names as its
/// reason is locked: never a candidate, and not counted as unused.
///
/// @pre `state.config.eviction_rate` is in `[0, 1]`.
DeletionPlan build_deletion_plan(const DeletionState& state);

} // namespace xorcle
