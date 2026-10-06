#include "heuristics/deletion/deletion.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <optional>

namespace xorcle {

namespace {

/// @brief Whether `left` is deleted before `right`, ordered totally so that a tie
/// does not depend on the sort.
bool deleted_first(const DeletionState& state, ClauseId left, ClauseId right) {
    if (state.key == DeletionKey::activity) {
        const double left_activity = state.activity->clause_activity(left);
        const double right_activity = state.activity->clause_activity(right);
        if (left_activity != right_activity) {
            return left_activity < right_activity;
        }
    } else {
        const double left_lbd = state.selector.clause_lbd(left);
        const double right_lbd = state.selector.clause_lbd(right);
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

bool is_deletion_candidate(const ClauseSelector& selector, ClauseId id, double wait) {
    assert(selector.is_clause_learned(id) && "a starting clause is never a deletion candidate");
    return selector.is_clause_out_of_unused_passes(id) ||
           allowed_unused_passes(selector.clause_lbd(id), wait) == 0;
}

void update_with_reason_use(ClauseSelector& selector, ClauseId id, std::size_t measured_lbd,
                            const DeletionConfig& config) {
    if (!selector.is_clause_learned(id)) {
        return;
    }
    selector.set_clause_used_since_deletion(id, true);
    const bool was_candidate = is_deletion_candidate(selector, id, config.wait);
    const double weight = 1.0 / static_cast<double>(config.lbd_window);
    selector.set_clause_lbd(id, (1.0 - weight) * selector.clause_lbd(id) +
                                    weight * static_cast<double>(measured_lbd));
    if (was_candidate) {
        selector.set_clause_out_of_unused_passes(id, false);
        selector.set_clause_unused_passes(id, 0);
    }
}

std::size_t conflicts_between_deletion_passes(const DeletionConfig& config,
                                              std::size_t num_conflicts) {
    const long gap =
        std::lround(config.cadence_coefficient * std::sqrt(static_cast<double>(num_conflicts)));
    return std::max<std::size_t>(1, static_cast<std::size_t>(gap));
}

DeletionPlan build_deletion_plan(const DeletionState& state) {
    assert((state.key != DeletionKey::activity || state.activity != nullptr) &&
           "the activity key has no activity source");
    assert(state.eviction_rate >= 0.0 && state.eviction_rate <= 1.0 &&
           "the eviction rate is not a fraction");

    const std::size_t num_clauses = state.selector.num_clauses();
    std::vector<std::uint8_t> is_locked(num_clauses, 0);
    for (const std::optional<ClauseId>& reason : state.trail.reasons()) {
        if (reason.has_value()) {
            is_locked[*reason] = 1;
        }
    }

    DeletionPlan plan;
    plan.unused_passes.resize(num_clauses);
    std::vector<ClauseId> candidates;

    for (const ClauseId id : state.selector.active_clause_ids()) {
        plan.unused_passes[id] = state.selector.clause_unused_passes(id);
        if (!state.selector.is_clause_learned(id) || is_locked[id] != 0) {
            continue;
        }
        if (is_deletion_candidate(state.selector, id, state.wait)) {
            candidates.push_back(id);
            continue;
        }
        if (state.selector.is_clause_used_since_deletion(id)) {
            plan.unused_passes[id] = 0;
            continue;
        }
        ++plan.unused_passes[id];
        const std::size_t allowed =
            allowed_unused_passes(state.selector.clause_lbd(id), state.wait);
        if (plan.unused_passes[id] >= allowed) {
            plan.out_of_unused_passes.push_back(id);
        }
    }

    const auto delete_count = static_cast<std::size_t>(
        std::floor(static_cast<double>(candidates.size()) * state.eviction_rate));
    std::sort(candidates.begin(), candidates.end(),
              [&](ClauseId left, ClauseId right) { return deleted_first(state, left, right); });
    plan.deleted.assign(candidates.begin(),
                        candidates.begin() + static_cast<std::ptrdiff_t>(delete_count));
    std::sort(plan.deleted.begin(), plan.deleted.end());
    return plan;
}

} // namespace xorcle
