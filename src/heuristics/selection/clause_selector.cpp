#include "heuristics/selection/clause_selector.h"

#include <algorithm>
#include <cassert>
#include <utility>

#include "algebra/dense/dense_gf2_vector.h"
#include "heuristics/selection/cyclic.h"
#include "heuristics/selection/exact_watchlist.h"

namespace xorcle {

#ifndef NDEBUG
void ClauseSelector::debug_assert_database_synchronized() const {
    assert(clauses_.size() == clause_active_.size() &&
           "clause database and activity flags are desynchronized");
    assert(clauses_.size() == clause_learned_.size() &&
           clauses_.size() == clause_used_since_deletion_.size() &&
           clauses_.size() == clause_tier_.size() && clauses_.size() == clause_lbd_.size() &&
           clauses_.size() == clause_unused_passes_.size() &&
           clauses_.size() == clause_spans_eq_false_.size() &&
           clauses_.size() == clause_entered_.size() &&
           "clause database and metadata are desynchronized");
}

void ClauseSelector::debug_assert_clause_id(ClauseId id) const {
    debug_assert_database_synchronized();
    assert(id < clauses_.size() && "clause id is out of range");
}
#endif

Clause& ClauseSelector::clause(ClauseId id) {
#ifndef NDEBUG
    debug_assert_clause_id(id);
#endif
    return clauses_[id];
}

void ClauseSelector::replace_clause(ClauseId id, Clause clause) {
#ifndef NDEBUG
    debug_assert_clause_id(id);
#endif
    const Clause replaced = std::exchange(clauses_[id], std::move(clause));
    if (clause_active_[id] != 0 && clause_entered_[id] != 0) {
        assert(replaced.cols() == clauses_[id].cols() &&
               "an active clause was replaced by one over another numbering");
        enter_carried_col_idxs(id, &replaced);
        maybe_clean_carrier_entries();
    }
}

void ClauseSelector::enter_carried_col_idxs(const ClauseId id, const Clause* const replaced) {
    const IndexSet carried = clauses_[id].carried_col_idxs();
    const std::size_t constant_col_idx = carried.size() - 1;
    if (col_idx_to_clause_ids_.size() < constant_col_idx) {
        col_idx_to_clause_ids_.resize(constant_col_idx);
    }
    // Testing a column against the replaced clause's rows beats taking their union
    // until as many columns are tested as a row has words.
    std::size_t num_row_tests_left =
        (carried.size() + DenseGF2Vector::word_mask) >> DenseGF2Vector::word_shift;
    IndexSet replaced_carried;
    const auto replaced_carries = [&](const std::size_t col_idx) {
        if (num_row_tests_left == 0) {
            if (replaced_carried.size() == 0) {
                replaced_carried = replaced->carried_col_idxs();
            }
            return replaced_carried.test(col_idx);
        }
        --num_row_tests_left;
        return replaced->carries(col_idx);
    };
    carried.for_each_set([&](const std::size_t col_idx) {
        if (col_idx != constant_col_idx && !(replaced != nullptr && replaced_carries(col_idx))) {
            col_idx_to_clause_ids_[col_idx].push_back(id);
            ++num_carrier_entries_;
        }
    });
    clause_spans_eq_false_[id] = clauses_[id].pivot_col_idxs().test(constant_col_idx) ? 1 : 0;
    if (spans_eq_false(id)) {
        clause_ids_spanning_eq_false_.push_back(id);
        ++num_carrier_entries_;
    }
    peak_carrier_entries_ = std::max(peak_carrier_entries_, num_carrier_entries_);
}

void ClauseSelector::maybe_clean_carrier_entries() {
    if (num_carrier_entries_ <= carrier_cleanup_threshold_) {
        return;
    }
    IndexSet kept_ids(clauses_.size()); // over clause ids; empty between lists
    const auto clean = [&kept_ids](std::vector<ClauseId>& ids, const auto& is_right) {
        std::erase_if(ids, [&](ClauseId id) {
            if (kept_ids.test(id) || !is_right(id)) {
                return true;
            }
            kept_ids.set(id);
            return false;
        });
        for (const ClauseId id : ids) {
            kept_ids.reset(id);
        }
        return ids.size();
    };
    std::size_t kept = clean(clause_ids_spanning_eq_false_,
                             [this](ClauseId id) { return spans_eq_false(id); });
    for (std::size_t col_idx = 0; col_idx < col_idx_to_clause_ids_.size(); ++col_idx) {
        kept += clean(col_idx_to_clause_ids_[col_idx],
                      [this, col_idx](ClauseId id) { return carries(id, col_idx); });
    }
    num_carrier_entries_ = kept;
    carrier_cleanup_threshold_ = 2 * kept + col_idx_to_clause_ids_.size();
}

std::vector<ClauseId> ClauseSelector::clause_ids_carrying(const IndexSet& col_idxs) {
    for (const ClauseId id : unentered_clause_ids_) {
        if (clause_active_[id] != 0) {
            clause_entered_[id] = 1;
            enter_carried_col_idxs(id, nullptr);
        }
    }
    unentered_clause_ids_.clear();
    maybe_clean_carrier_entries();

    IndexSet found(clauses_.size()); // over clause ids
    col_idxs.for_each_set([&](const std::size_t col_idx) {
        if (col_idx >= col_idx_to_clause_ids_.size()) {
            return;
        }
        for (const ClauseId id : col_idx_to_clause_ids_[col_idx]) {
            if (!found.test(id) && carries(id, col_idx)) {
                found.set(id);
            }
        }
    });
    for (const ClauseId id : clause_ids_spanning_eq_false_) {
        if (spans_eq_false(id)) {
            found.set(id);
        }
    }
    std::vector<ClauseId> ids;
    found.for_each_set([&ids](const std::size_t id) { ids.push_back(id); });
    assert(ids == debug_scan_clause_ids_carrying(col_idxs) &&
           "the column-to-clause entries miss a clause or name a wrong one");
    return ids;
}

#ifndef NDEBUG
std::vector<ClauseId> ClauseSelector::debug_scan_clause_ids_carrying(
    const IndexSet& col_idxs) const {
    std::vector<ClauseId> ids;
    for (const ClauseId id : active_clause_ids()) {
        const Clause& clause = clauses_[id];
        if (clause.pivot_col_idxs().test(clause.cols() - 1) ||
            clause.carried_col_idxs().intersects(col_idxs)) {
            ids.push_back(id);
        }
    }
    return ids;
}
#endif

bool ClauseSelector::is_clause_active(ClauseId id) const {
#ifndef NDEBUG
    debug_assert_clause_id(id);
#endif
    return clause_active_[id] != 0;
}

void ClauseSelector::set_clause_active(ClauseId id, bool active) {
#ifndef NDEBUG
    debug_assert_clause_id(id);
#endif
    assert((!active || clause_active_[id] != 0) && "an inactive clause was activated");
    if (active || clause_active_[id] == 0) {
        return;
    }
    clause_active_[id] = 0;
    if (2 * ++num_inactive_listed_ > listed_clause_ids_.size()) {
        std::erase_if(listed_clause_ids_,
                      [this](ClauseId listed) { return clause_active_[listed] == 0; });
        num_inactive_listed_ = 0;
    }
    update_with_inactive_clause(id);
}

void ClauseSelector::set_clause_learned(ClauseId id, bool learned) {
#ifndef NDEBUG
    debug_assert_clause_id(id);
#endif
    clause_learned_[id] = learned ? 1 : 0;
}

bool ClauseSelector::is_clause_learned(ClauseId id) const {
#ifndef NDEBUG
    debug_assert_clause_id(id);
#endif
    return clause_learned_[id] != 0;
}

std::size_t ClauseSelector::clause_lbd(ClauseId id) const {
#ifndef NDEBUG
    debug_assert_clause_id(id);
#endif
    return clause_lbd_[id];
}

void ClauseSelector::set_clause_lbd(ClauseId id, std::size_t lbd) {
#ifndef NDEBUG
    debug_assert_clause_id(id);
#endif
    clause_lbd_[id] = lbd;
}

ClauseTier ClauseSelector::clause_tier(ClauseId id) const {
#ifndef NDEBUG
    debug_assert_clause_id(id);
#endif
    return clause_tier_[id];
}

void ClauseSelector::set_clause_tier(ClauseId id, ClauseTier tier) {
#ifndef NDEBUG
    debug_assert_clause_id(id);
#endif
    clause_tier_[id] = tier;
}

bool ClauseSelector::is_clause_used_since_deletion(ClauseId id) const {
#ifndef NDEBUG
    debug_assert_clause_id(id);
#endif
    return clause_used_since_deletion_[id] != 0;
}

void ClauseSelector::set_clause_used_since_deletion(ClauseId id, bool used) {
#ifndef NDEBUG
    debug_assert_clause_id(id);
#endif
    clause_used_since_deletion_[id] = used ? 1 : 0;
}

std::size_t ClauseSelector::clause_unused_passes(ClauseId id) const {
#ifndef NDEBUG
    debug_assert_clause_id(id);
#endif
    return clause_unused_passes_[id];
}

void ClauseSelector::set_clause_unused_passes(ClauseId id, std::size_t num_passes) {
#ifndef NDEBUG
    debug_assert_clause_id(id);
#endif
    clause_unused_passes_[id] = num_passes;
}

void ClauseSelector::set_tier_config(const ClauseTierConfig& config) {
    assert(clauses_.empty() && "the tier cutoffs must be set before any clause is added");
    assert(config.core_lbd <= config.secondary_lbd &&
           "the core cutoff must not exceed the secondary cutoff");
    tier_config_ = config;
}

ClauseTier ClauseSelector::tier_for_lbd(std::size_t lbd) const {
    if (lbd <= tier_config_.core_lbd) {
        return ClauseTier::core;
    }
    return lbd <= tier_config_.secondary_lbd ? ClauseTier::secondary : ClauseTier::local;
}

void ClauseSelector::add_starting_clause(Clause clause) {
    add_clause(std::move(clause));
}

void ClauseSelector::add_asserting_clause(Clause clause, const Trail&,
                                          const AssertingClauseInfo& info) {
    add_clause(std::move(clause));
    const ClauseId id = clauses_.size() - 1;
    clause_learned_[id] = 1;
    clause_lbd_[id] = info.lbd;
    clause_tier_[id] = tier_for_lbd(info.lbd);
}

void ClauseSelector::update_with_propagation(const Equation& reduced_eqn, const Trail& trail) {
    update_with_new_equation(reduced_eqn, trail);
}

void ClauseSelector::update_with_decision(const Equation& reduced_eqn, const Trail& trail) {
    update_with_new_equation(reduced_eqn, trail);
}

void ClauseSelector::update_with_new_watch(ClauseId, ClauseRowIdx, WatchType, const Trail&) {}

void ClauseSelector::update_with_backjump(std::size_t, const Trail&) {}

void ClauseSelector::update_with_substitution(std::span<const ClauseId>, const IndexSet&,
                                              const Trail&) {}

void ClauseSelector::update_with_renumbering(const CompactionTransform& transform, const Trail&) {
    for (const ClauseId id : active_clause_ids()) {
#ifndef NDEBUG
        clauses_[id].carried_col_idxs().for_each_set([&](const std::size_t col_idx) {
            assert((col_idx == transform.old_num_vars() ||
                    transform.to_new(col_idx) != CompactionTransform::npos) &&
                   "a clause carries a dropped column at a renumbering");
        });
#endif
        clauses_[id].compact(transform);
    }
    // Each column's entries move to its new place in order; a dropped column's are stale.
    for (std::size_t col_idx = 0; col_idx < col_idx_to_clause_ids_.size(); ++col_idx) {
        const std::size_t new_col_idx = transform.to_new(col_idx);
        if (new_col_idx == CompactionTransform::npos) {
            num_carrier_entries_ -= col_idx_to_clause_ids_[col_idx].size();
            col_idx_to_clause_ids_[col_idx].clear();
        } else if (new_col_idx != col_idx) {
            col_idx_to_clause_ids_[new_col_idx] = std::move(col_idx_to_clause_ids_[col_idx]);
            col_idx_to_clause_ids_[col_idx].clear();
        }
    }
    col_idx_to_clause_ids_.resize(transform.new_num_vars());
}

void ClauseSelector::add_to_stats(RunStats& stats) const {
    stats.peak_carrier_entries = peak_carrier_entries_;
}

void ClauseSelector::add_clause(Clause clause) {
    listed_clause_ids_.push_back(clauses_.size());
    const ClauseId id = clauses_.size();
    clause_spans_eq_false_.push_back(0);
    clauses_.push_back(std::move(clause));
    clause_active_.push_back(1);
    clause_entered_.push_back(0);
    unentered_clause_ids_.push_back(id);
    clause_learned_.push_back(0);
    clause_used_since_deletion_.push_back(0);
    // A starting clause enters `local` with no LBD: it is tiered by its first use
    // and, being unlearned, is never a deletion candidate.
    clause_tier_.push_back(ClauseTier::local);
    clause_lbd_.push_back(unset_clause_lbd);
    clause_unused_passes_.push_back(0);
}

void ClauseSelector::update_with_new_equation(const Equation&, const Trail&) {}

void ClauseSelector::update_with_inactive_clause(ClauseId) {}

std::unique_ptr<ClauseSelector> make_clause_selector(ClauseSelectorName name,
                                                     std::size_t num_vars) {
    switch (name) {
        case ClauseSelectorName::cyclic:
            return std::make_unique<Cyclic>(num_vars);
        case ClauseSelectorName::exact_watchlist:
            return std::make_unique<ExactWatchlist>(num_vars);
    }
    assert(false && "make_clause_selector: unknown clause selector name");
    std::unreachable();
}

} // namespace xorcle
