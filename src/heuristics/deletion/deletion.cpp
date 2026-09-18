#include "heuristics/deletion/deletion.h"

#include <algorithm>
#include <cassert>
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
        const std::size_t left_lbd = state.selector.clause_lbd(left);
        const std::size_t right_lbd = state.selector.clause_lbd(right);
        if (left_lbd != right_lbd) {
            return left_lbd > right_lbd;
        }
    }
    return left < right;
}

} // namespace

DeletionPlan build_deletion_plan(const DeletionState& state) {
    assert((state.key != DeletionKey::activity || state.activity != nullptr) &&
           "the activity key has no activity source");

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

    for (ClauseId id = 0; id < num_clauses; ++id) {
        plan.unused_passes[id] = state.selector.clause_unused_passes(id);
        // A starting clause is tiered and demoted like a learned one; only the
        // deletion candidates below are restricted to learned clauses.
        if (!state.selector.is_clause_active(id) || is_locked[id] != 0) {
            continue;
        }
        switch (state.selector.clause_tier(id)) {
            case ClauseTier::core:
                break;
            case ClauseTier::secondary:
                if (state.selector.is_clause_used_since_deletion(id)) {
                    plan.unused_passes[id] = 0;
                } else if (plan.unused_passes[id] + 1 >= state.demote_cadence_in_passes) {
                    plan.demoted.push_back(id);
                    plan.unused_passes[id] = 0;
                } else {
                    ++plan.unused_passes[id];
                }
                break;
            case ClauseTier::local:
                if (state.selector.is_clause_learned(id)) {
                    candidates.push_back(id);
                }
                break;
        }
    }

    const std::size_t delete_count = candidates.size() / 2;
    std::sort(candidates.begin(), candidates.end(),
              [&](ClauseId left, ClauseId right) { return deleted_first(state, left, right); });
    plan.deleted.assign(candidates.begin(), candidates.begin() + delete_count);
    std::sort(plan.deleted.begin(), plan.deleted.end());
    return plan;
}

} // namespace xorcle
