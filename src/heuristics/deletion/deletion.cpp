#include "heuristics/deletion/deletion.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>

#include "util/index_set.h"

namespace xorcle {

namespace {

/// @brief Whether `left` is deleted before `right`, ordered totally so that a tie
/// does not depend on the sort.
bool deleted_first(const DeletionState& state, ClauseId left, ClauseId right) {
    if (state.config.key == DeletionKey::activity) {
        const double left_activity = state.activity.clause_activity(left);
        const double right_activity = state.activity.clause_activity(right);
        if (left_activity != right_activity) {
            return left_activity < right_activity;
        }
    } else {
        const double left_lbd = state.database.record(left).lbd;
        const double right_lbd = state.database.record(right).lbd;
        if (left_lbd != right_lbd) {
            return left_lbd > right_lbd;
        }
    }
    return left < right;
}

} // namespace

std::size_t allowed_unused_passes(double lbd, double wait) {
    return static_cast<std::size_t>(std::lround(std::pow(2.0, wait - lbd)));
}

void update_with_reason_use(ClauseRecord& record) {
    if (record.learned) {
        record.used_since_deletion = true;
    }
}

std::size_t conflicts_between_deletion_passes(const DeletionConfig& config,
                                              std::size_t num_conflicts) {
    const long gap = std::lround(config.cadence_coefficient *
                                 std::sqrt(static_cast<double>(num_conflicts) +
                                           static_cast<double>(config.cadence_offset)));
    return std::max<std::size_t>(1, static_cast<std::size_t>(gap));
}

DeletionPlan build_deletion_plan(const DeletionState& state) {
    assert(state.config.eviction_rate >= 0.0 && state.config.eviction_rate <= 1.0 &&
           "the eviction rate is not a fraction");

    const IndexSet is_locked = state.trail.reason_clause_ids(state.database.num_clauses());

    DeletionPlan plan;
    plan.unused_passes.resize(state.database.active_clause_ids().max_size());
    std::size_t num_planned = 0;
    std::vector<ClauseId> candidates;

    for (const ClauseId id : state.database.active_clause_ids()) {
        const ClauseRecord& record = state.database.record(id);
        std::size_t& unused_passes = plan.unused_passes[num_planned++];
        unused_passes = record.unused_passes;
        if (!record.learned) {
            continue;
        }
        assert(record.lbd_measured && "a learned clause has no LBD");
        if (record.used_since_deletion) {
            unused_passes = 0;
            continue;
        }
        if (is_locked.test(id)) {
            continue;
        }
        if (unused_passes >= allowed_unused_passes(record.lbd, state.config.wait)) {
            candidates.push_back(id);
            continue;
        }
        ++unused_passes;
    }

    plan.unused_passes.resize(num_planned);

    const auto delete_count = static_cast<std::size_t>(
        std::floor(static_cast<double>(candidates.size()) * state.config.eviction_rate));
    std::sort(candidates.begin(), candidates.end(),
              [&](ClauseId left, ClauseId right) { return deleted_first(state, left, right); });
    plan.deleted.assign(candidates.begin(),
                        candidates.begin() + static_cast<std::ptrdiff_t>(delete_count));
    std::sort(plan.deleted.begin(), plan.deleted.end());
    return plan;
}

} // namespace xorcle
