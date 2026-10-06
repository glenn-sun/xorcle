#include "heuristics/selection/clause_selector.h"

#include <algorithm>
#include <cassert>
#include <utility>

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
           clauses_.size() == clause_carried_col_idxs_.size() &&
           clauses_.size() == clause_spans_eq_false_.size() &&
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
    const IndexSet carried_before = std::move(clause_carried_col_idxs_[id]);
    clause_carried_col_idxs_[id] =
        clause_active_[id] != 0 ? clause.carried_col_idxs() : IndexSet();
    clauses_[id] = std::move(clause);
    if (clause_active_[id] != 0) {
        enter_carried_col_idxs(id, carried_before);
        maybe_clean_carrier_entries();
    }
}

void ClauseSelector::enter_carried_col_idxs(const ClauseId id, const IndexSet& entered_col_idxs) {
    const IndexSet& carried = clause_carried_col_idxs_[id];
    const std::size_t constant_col_idx = carried.size() - 1;
    if (col_idx_to_clause_ids_.size() < constant_col_idx) {
        col_idx_to_clause_ids_.resize(constant_col_idx);
    }
    carried.for_each_set([&](const std::size_t col_idx) {
        if (col_idx != constant_col_idx &&
            !(col_idx < entered_col_idxs.size() && entered_col_idxs.test(col_idx))) {
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
    const auto clean = [](std::vector<ClauseId>& ids, const auto& is_stale) {
        std::erase_if(ids, is_stale);
        std::ranges::sort(ids);
        const auto repeated = std::ranges::unique(ids);
        ids.erase(repeated.begin(), repeated.end());
        return ids.size();
    };
    std::size_t kept = clean(clause_ids_spanning_eq_false_,
                             [this](ClauseId id) { return !spans_eq_false(id); });
    for (std::size_t col_idx = 0; col_idx < col_idx_to_clause_ids_.size(); ++col_idx) {
        kept += clean(col_idx_to_clause_ids_[col_idx],
                      [this, col_idx](ClauseId id) { return !carries(id, col_idx); });
    }
    num_carrier_entries_ = kept;
    carrier_cleanup_threshold_ = 2 * kept + col_idx_to_clause_ids_.size();
}

std::vector<ClauseId> ClauseSelector::clause_ids_carrying(const IndexSet& col_idxs) const {
    std::vector<ClauseId> ids;
    col_idxs.for_each_set([&](const std::size_t col_idx) {
        if (col_idx >= col_idx_to_clause_ids_.size()) {
            return;
        }
        for (const ClauseId id : col_idx_to_clause_ids_[col_idx]) {
            if (carries(id, col_idx)) {
                ids.push_back(id);
            }
        }
    });
    for (const ClauseId id : clause_ids_spanning_eq_false_) {
        if (spans_eq_false(id)) {
            ids.push_back(id);
        }
    }
    std::ranges::sort(ids);
    const auto repeated = std::ranges::unique(ids);
    ids.erase(repeated.begin(), repeated.end());
    assert(ids == debug_scan_clause_ids_carrying(col_idxs) &&
           "the column-to-clause entries miss a clause or name a wrong one");
    return ids;
}

#ifndef NDEBUG
std::vector<ClauseId> ClauseSelector::debug_scan_clause_ids_carrying(
    const IndexSet& col_idxs) const {
    std::vector<ClauseId> ids;
    for (const ClauseId id : active_clause_ids()) {
        assert(clause_carried_col_idxs_[id] == clauses_[id].carried_col_idxs() &&
               "a clause's carried columns are stale");
        const Clause& clause = clauses_[id];
        if (clause.pivot_col_idxs().test(clause.cols() - 1) ||
            clause_carried_col_idxs_[id].intersects(col_idxs)) {
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
    clause_carried_col_idxs_[id] = IndexSet();
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
    for (std::vector<ClauseId>& ids : col_idx_to_clause_ids_) {
        ids.clear();
    }
    col_idx_to_clause_ids_.resize(transform.new_num_vars());
    clause_ids_spanning_eq_false_.clear();
    num_carrier_entries_ = 0;
    for (const ClauseId id : active_clause_ids()) {
        clauses_[id].compact(transform);
        clause_carried_col_idxs_[id] = clauses_[id].carried_col_idxs();
        enter_carried_col_idxs(id, IndexSet());
    }
    carrier_cleanup_threshold_ = 2 * num_carrier_entries_ + col_idx_to_clause_ids_.size();
}

void ClauseSelector::add_to_stats(RunStats& stats) const {
    stats.peak_carrier_entries = peak_carrier_entries_;
}

void ClauseSelector::add_clause(Clause clause) {
    listed_clause_ids_.push_back(clauses_.size());
    const ClauseId id = clauses_.size();
    clause_carried_col_idxs_.push_back(clause.carried_col_idxs());
    clause_spans_eq_false_.push_back(0);
    clauses_.push_back(std::move(clause));
    clause_active_.push_back(1);
    enter_carried_col_idxs(id, IndexSet());
    maybe_clean_carrier_entries();
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
