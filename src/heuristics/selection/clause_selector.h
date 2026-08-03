#pragma once

/// @file clause_selector.h
/// @brief Choosing which clause to propagate from next, and owning the clause
/// database while doing it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "core/watch.h"

namespace xorcle {

/// @brief A clause selected for propagation, with an optional watch hint.
///
/// A selector that tracks reductions itself can tell the propagation engine
/// which trigger fired, sparing it the recomputation. Unit clauses must carry no
/// hint.
struct ClauseSelection {
    ClauseId id; ///< The clause to try propagating from.
    std::optional<WatchType> zero_watch_hint; ///< The trigger, if the selector knows it.
};

/// @brief Available clause-selection strategies.
///
/// @note The 0.1 tree also had `exact_watchlist`. It is not ported: it was
/// complex and experiments did not show it paying for its overhead.
enum class ClauseSelectorName {
    cyclic, ///< Round-robin over the clause database.
    leading_term_watchlist ///< Work queue driven by the leading terms of watched rows.
};

/// @brief Decides which clause the solver tries to propagate from next.
///
/// Unit propagation over a *set* of clauses says only "choosing clauses @f$C \in
/// \Delta@f$"; `core/propagation.h` propagates one given clause and this picks
/// it.
///
/// @note This is also the **authoritative clause store**. The solver keeps no
/// clause vector of its own: it adds clauses here, reads them back through
/// `clause()`, and compaction writes the rewritten clauses back through the
/// same accessor. That is not incidental — a watchlist selector's indexes are a
/// view of this storage, and splitting the two would let them drift.
///
/// A selector is also the propagation engine's `WatchObserver`, since a
/// watchlist keyed on watched rows has to be told when one moves.
class ClauseSelector : public WatchObserver {
public:
    /// @brief Construct an empty selector.
    ClauseSelector() = default;

    ClauseSelector(const ClauseSelector&) = delete;
    ClauseSelector& operator=(const ClauseSelector&) = delete;

    /// @brief Virtual destructor for interface use.
    ~ClauseSelector() override = default;

    /// @brief Number of clauses in the database.
    std::size_t num_clauses() const noexcept { return clauses_.size(); }

    /// @brief The clause database, in clause-id order.
    const std::vector<Clause>& clauses() const noexcept { return clauses_; }

    /// @brief A clause, by id.
    /// @param id The clause to fetch.
    /// @pre `id < num_clauses()`.
    /// @note Mutable because the propagation engine moves the clause's watches.
    Clause& clause(ClauseId id);

    /// @brief A clause, by id.
    /// @param id The clause to fetch.
    /// @pre `id < num_clauses()`.
    const Clause& clause(ClauseId id) const;

    /// @brief Whether a clause is still active.
    /// @param id The clause to test.
    /// @pre `id < num_clauses()`.
    bool is_clause_active(ClauseId id) const;

    /// @brief Mark a clause active or inactive.
    /// @param id The clause to update.
    /// @param active Whether it remains active.
    /// @pre `id < num_clauses()`.
    void set_clause_active(ClauseId id, bool active);

    /// @brief Per-clause active flags, in clause-id order.
    /// @post The result has size `num_clauses()`.
    const std::vector<std::uint8_t>& active_flags() const noexcept { return clause_active_; }

    /// @brief The next clause to try, or `std::nullopt` once propagation is quiescent.
    virtual std::optional<ClauseId> next() = 0;

    /// @brief The next clause to try, with a watch hint when the selector has one.
    /// @note The default drops the hint and delegates to `next()`.
    virtual std::optional<ClauseSelection> next_with_hint();

    /// @brief Add a clause from the input instance.
    /// @param clause The clause to add; it takes the next id.
    virtual void add_starting_clause(Clause clause);

    /// @brief Add a learned asserting clause, after the trail has been backjumped.
    /// @param clause The asserting clause; it takes the next id.
    /// @param trail The trail, already reflecting the backjump.
    /// @param removed_levels How many decision levels the solver just discarded.
    /// @pre `trail` already reflects the backjumped state.
    /// @note This really is the number of levels *removed*, not the asserting
    /// level and not its complement: after a restart the solver discards every
    /// non-root level regardless of where the clause asserts, so the two differ.
    /// The 0.1 tree named this parameter `jumpable_levels` while passing the
    /// removed count, which was simply a misnomer.
    virtual void add_asserting_clause(Clause clause, const Trail& trail,
                                      std::size_t removed_levels);

    /// @brief Note that an equation was learned by propagation.
    /// @param reduced_eqn The learned equation, already reduced by the span it was added to.
    /// @param trail The trail, already holding `reduced_eqn`.
    virtual void update_with_propagation(const Equation& reduced_eqn, const Trail& trail);

    /// @brief Note that an equation was learned as a decision.
    /// @param reduced_eqn The decision equation, already reduced by the span it was added to.
    /// @param trail The trail, already holding `reduced_eqn`.
    virtual void update_with_decision(const Equation& reduced_eqn, const Trail& trail);

    /// @brief Note the clause the decision procedure decided from.
    /// @param clause_id The clause to prioritize, or `std::nullopt`.
    virtual void update_with_decision_clause(std::optional<ClauseId> clause_id);

    /// @brief Note that the propagation engine moved a watch.
    /// @note The default ignores it; only a watchlist selector cares.
    void update_with_new_watch(ClauseId id, EquationId old_watch, WatchType type,
                               const Trail& trail) override;

    /// @brief Note that every clause has been rewritten into a compacted variable space.
    /// @param transform The compaction step that was applied.
    /// @param trail The trail, already reset for the compacted space.
    virtual void update_with_compaction(const CompactionTransform& transform, const Trail& trail);

protected:
    /// @brief Store a clause, with no other side effects.
    /// @param clause The clause to store; it takes the next id.
    virtual void add_clause(Clause clause);

    /// @brief React to a newly learned equation, propagated or decided alike.
    /// @param reduced_eqn The learned equation, already reduced.
    /// @param trail The trail, already holding it.
    virtual void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail);

#ifndef NDEBUG
    /// @brief Assert that the clause vector and the active flags agree in size.
    void debug_assert_storage_synchronized() const;
#endif

    /// @brief The clause database; a clause's id is its index here.
    std::vector<Clause> clauses_;

    /// @brief Per-clause active flags, parallel to `clauses_`.
    std::vector<std::uint8_t> clause_active_;
};

} // namespace xorcle
