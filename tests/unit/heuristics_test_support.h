#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>
#include <vector>

#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "algebra/compaction_transform.h"
#include "core/clause_database.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "core/solver.h"
#include "core/watch.h"
#include "heuristics/bve/bve.h"
#include "heuristics/decisions/csids.h"
#include "heuristics/deletion/deletion.h"
#include "heuristics/implication_graph/implication_graph.h"
#include "heuristics/restarts/restart_policy.h"
#include "heuristics/selection/clause_selector.h"

namespace xorcle {

/// @brief Each configuration as an empty command line gives it.
CsidsConfig default_csids_config();
RestartConfig default_restart_config();
ImplicationGraphConfig default_implication_graph_config();
BveConfig default_bve_config();
DeletionConfig default_deletion_config();
SolverConfig default_solver_config();

/// @brief `default_csids_config()` deciding in @p phase, without phase saving.
CsidsConfig csids_config_deciding(DecisionPhase phase);

/// @brief Build a clause from its rows, each given as a list of set bits in the
/// linear negation, where bit `num_vars` is the constant bit.
Clause make_clause(EquationRepr repr, std::size_t num_vars,
                   std::initializer_list<std::initializer_list<std::size_t>> rows);

/// @brief Build a row space from its rows, given as lists of set bits.
RowSpace make_row_space(EquationRepr repr, std::size_t num_vars,
                        std::initializer_list<std::initializer_list<std::size_t>> rows);

/// @brief How many dimensions `clause` adds to `span`; the clause is asserting
/// exactly when this is 1.
std::size_t dimensions_outside_span(const Clause& clause, const RowSpace& span);

/// @brief All `2^num_vars` sign patterns over `num_vars` variables in DIMACS:
/// UNSAT, and every assignment is ruled out by exactly one clause, so the
/// search must decide.
std::string sign_pattern_instance(std::size_t num_vars);

/// @brief A conflicting trail together with the clause database that built it.
struct Conflict {
    Trail trail;
    std::vector<Clause> clauses; ///< The clause database, in clause-id order.

    /// @brief The clause whose propagation made the trail conflicting.
    ClauseId conflict_clause_id() const { return *trail.reasons().back(); }
};

class IgnoreWatchUpdates final : public WatchUpdateHandler {
public:
    HeldReduction copy_reduced_companion(ClauseId, WatchType, Equation&, ReduceResult&) override {
        return HeldReduction::none;
    }
    void update_with_new_watch(ClauseId, ClauseRowIdx, WatchType, const Trail&,
                               const WatchReductions&) override {}
};

/// @brief Report to `handler` that a watch of `clause` moved, with the reductions
/// of its watches by `trail` that the propagator would hand over.
void report_new_watch(WatchUpdateHandler& handler, ClauseId id, const Clause& clause,
                      const ClauseWatches& watches, ClauseRowIdx old_watch_row_idx, WatchType type,
                      const Trail& trail);

/// @brief Add a starting clause to `database` and tell `selector`, as the solver does.
ClauseId add_starting_clause(ClauseDatabase& database, ClauseSelector& selector, Clause clause);

/// @brief Add a learned clause to `database` and tell `selector`, as the solver does.
ClauseId add_asserting_clause(ClauseDatabase& database, ClauseSelector& selector, Clause clause,
                              const Trail& trail, const AssertingClauseInfo& info);

/// @brief Deactivate a clause of `database` and tell `selector`, as the solver does.
void deactivate_clause(ClauseDatabase& database, ClauseSelector& selector, ClauseId id);

/// @brief Renumber `database` and tell `selector`, as the solver does.
void renumber(ClauseDatabase& database, ClauseSelector& selector,
              const CompactionTransform& transform, const Trail& trail);

} // namespace xorcle
