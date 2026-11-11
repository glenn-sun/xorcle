#include "heuristics/selection/clause_selector.h"

#include <cassert>
#include <utility>

#include "algebra/gf2_vector_of_repr.h"
#include "heuristics/selection/cyclic.h"
#include "heuristics/selection/exact_watchlist.h"

namespace xorcle {

void ClauseSelector::update_with_new_clause(ClauseId) {}

void ClauseSelector::update_with_asserting_clause(const ClauseId id, const Trail&,
                                                  const AssertingClauseInfo&) {
    update_with_new_clause(id);
}

void ClauseSelector::update_with_inactive_clause(ClauseId) {}

void ClauseSelector::update_with_narrowed_clause(ClauseId) {}

void ClauseSelector::update_with_new_equation(const Equation&, const Trail&) {}

void ClauseSelector::update_with_decision(const Equation& reduced_eqn, const Trail& trail,
                                          std::optional<ClauseId>) {
    update_with_new_equation(reduced_eqn, trail);
}

HeldReduction ClauseSelector::copy_reduced_companion(ClauseId, WatchType, Equation&,
                                                     ReduceResult&) {
    return HeldReduction::none;
}

void ClauseSelector::update_with_new_watch(ClauseId, ClauseRowIdx, WatchType, const Trail&,
                                           const WatchReductions&) {}

void ClauseSelector::update_with_backjump(std::size_t, const Trail&) {}

void ClauseSelector::update_with_substitution(std::span<const ClauseId>, const IndexSet&,
                                              const Trail&) {}

void ClauseSelector::update_with_renumbering(const CompactionTransform&, const Trail&) {}

std::unique_ptr<ClauseSelector> make_clause_selector(ClauseSelectorName name, EquationRepr repr,
                                                     std::size_t num_vars,
                                                     const ClauseDatabase& database) {
    switch (name) {
        case ClauseSelectorName::cyclic:
            return std::make_unique<Cyclic>(database);
        case ClauseSelectorName::exact_watchlist:
            return with_gf2_vector_of(
                repr, [&]<GF2Vector V>(std::type_identity<V>) -> std::unique_ptr<ClauseSelector> {
                    return std::make_unique<ExactWatchlistTemplate<V>>(num_vars, database);
                });
    }
    assert(false && "make_clause_selector: unknown clause selector name");
    std::unreachable();
}

} // namespace xorcle
