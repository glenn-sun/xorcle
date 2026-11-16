#pragma once

/// @file clause_selector.h
/// @brief Choosing which clause to propagate from next.

#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_database.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "core/watch.h"
#include "util/index_set.h"

namespace xorcle {

/// @brief A clause to try, and the trigger the selector already knows holds.
struct ClauseSelection {
    ClauseId id;
    std::optional<WatchType> trigger_hint;
};

enum class ClauseSelectorName {
    cyclic, ///< Round-robin over the clause database.
    exact_watchlist ///< Work queue of the clauses whose watch pair became dependent.
};

/// @brief What conflict analysis knows about a learned clause beyond its rows.
struct AssertingClauseInfo {
    std::size_t lbd;

    /// @brief The number of decision levels just *discarded*, which after a restart
    /// is neither the asserting level nor its complement.
    std::size_t removed_levels;

    /// @brief Whether this replaces a clause on-the-fly subsumption removed,
    /// rather than closing the conflict.
    bool is_replacement = false;
};

/// @brief Decides which clause of a `ClauseDatabase` the solver tries to propagate from next.
///
/// The solver changes the database and then tells the selector what changed.
class ClauseSelector : public WatchUpdateHandler {
public:
    /// @pre `database` holds no clause yet.
    explicit ClauseSelector(const ClauseDatabase& database) : database_(database) {
        assert(database.num_clauses() == 0 && "a selector was built over a database in use");
    }

    ClauseSelector(const ClauseSelector&) = delete;
    ClauseSelector& operator=(const ClauseSelector&) = delete;

    ~ClauseSelector() override = default;

    /// @brief The next clause to try, with the trigger when the selector knows it;
    /// `std::nullopt` once the propagation run is over.
    virtual std::optional<ClauseSelection> next() = 0;

    /// @brief Note a clause the database just added under @p id, other than a learned one.
    virtual void update_with_new_clause(ClauseId id);

    /// @brief Note a learned asserting clause the database just added under @p id;
    /// `trail` already reflects the backjump.
    virtual void update_with_asserting_clause(ClauseId id, const Trail& trail,
                                              const AssertingClauseInfo& info);

    /// @brief Note that an active clause went inactive, which the default ignores.
    virtual void update_with_inactive_clause(ClauseId id);

    /// @brief Note that the database overwrote an active clause at the root with a
    /// narrowing of it, which takes the place of its watches as a new clause would;
    /// the default ignores it.
    /// @pre The trail is at the root and at a fixed point.
    virtual void update_with_narrowed_clause(ClauseId id);

    /// @brief Note an equation learned by propagation, already reduced by the span it was
    /// added to and already on `trail`.
    virtual void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail);

    /// @brief Note a decision as `update_with_new_equation()` notes a propagation.
    /// @param decision_clause The clause the decision was drawn from, if any.
    virtual void update_with_decision(const Equation& reduced_eqn, const Trail& trail,
                                      std::optional<ClauseId> decision_clause);

    /// @brief The default holds no reduction.
    HeldReduction copy_reduced_companion(ClauseId id, WatchType type, Equation& reduced,
                                         ReduceResult& result) override;

    /// @brief Note a watch move, which the default ignores.
    void update_with_new_watch(ClauseId id, ClauseRowIdx old_watch_row_idx, WatchType type,
                               const Trail& trail, const WatchReductions& reductions) override;

    /// @brief Note that the trail dropped every level above `level`, which the
    /// default ignores; `trail` already reflects it.
    virtual void update_with_backjump(std::size_t level, const Trail& trail);

    /// @brief Note that a substitution emptied the root trail.
    /// @param rewritten_clause_ids The clauses it replaced or deactivated; every
    /// other active clause carries no pivot of the trail it emptied.
    /// @param dropped_col_idxs The variables dropped since the last renumbering.
    virtual void update_with_substitution(std::span<const ClauseId> rewritten_clause_ids,
                                          const IndexSet& dropped_col_idxs, const Trail& trail);

    /// @brief Note that the database renumbered every active clause, which moves no watch
    /// and which the default ignores.
    /// @pre `trail` is empty, and `update_with_substitution()` told of the last variable dropped.
    virtual void update_with_renumbering(const CompactionTransform& transform, const Trail& trail);

protected:
    const ClauseDatabase& database_;
};

/// @brief Builds a selector over the database it is given.
using ClauseSelectorFactory =
    std::function<std::unique_ptr<ClauseSelector>(const ClauseDatabase& database)>;

/// @brief Build the selector named by @p name over @p database, whose clauses
/// have @p num_vars variables and equations of @p repr.
std::unique_ptr<ClauseSelector> make_clause_selector(ClauseSelectorName name, EquationRepr repr,
                                                     std::size_t num_vars,
                                                     const ClauseDatabase& database);

} // namespace xorcle
