#include "heuristics/selection/clause_selector.h"

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
    clause_active_[id] = active ? 1 : 0;
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

void ClauseSelector::update_with_compaction(const CompactionTransform&, const Trail&) {}

void ClauseSelector::add_to_stats(RunStats&) const {}

void ClauseSelector::add_clause(Clause clause) {
    clauses_.push_back(std::move(clause));
    clause_active_.push_back(1);
    clause_learned_.push_back(0);
    clause_used_since_deletion_.push_back(0);
    // A starting clause enters `local` with no LBD: it is tiered by its first use
    // and, being unlearned, is never a deletion candidate.
    clause_tier_.push_back(ClauseTier::local);
    clause_lbd_.push_back(unset_clause_lbd);
    clause_unused_passes_.push_back(0);
}

void ClauseSelector::update_with_new_equation(const Equation&, const Trail&) {}

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
