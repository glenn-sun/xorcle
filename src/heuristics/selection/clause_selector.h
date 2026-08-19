#pragma once

/// @file clause_selector.h
/// @brief Choosing which clause to propagate from next, and owning the clause database.

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

/// @brief A clause selected for propagation. A selector that tracks reductions
/// itself can name the trigger that fired, sparing the propagator the
/// recomputation; unit clauses carry no hint.
struct ClauseSelection {
    ClauseId id;
    std::optional<WatchType> zero_watch_hint; ///< The trigger, if the selector knows it.
};

enum class ClauseSelectorName {
    cyclic, ///< Round-robin over the clause database.
    leading_term_watchlist ///< Work queue driven by the leading terms of watched rows.
};

/// @brief Decides which clause the solver tries to propagate from next;
/// `core/propagation.h` propagates the clause this picks. It is also the
/// **authoritative clause store**: the solver keeps no clause vector of its own
/// but adds clauses here, reads them back through `clause()`, and has compaction
/// write rewritten clauses back the same way. A watchlist selector's indexes are
/// a view of this storage, and splitting the two would let them drift. A
/// selector is likewise the propagator's `WatchUpdateHandler`, since a watchlist
/// keyed on watched rows must hear of a move.
///
/// @pre Every accessor taking a clause id requires `id < num_clauses()`.
class ClauseSelector : public WatchUpdateHandler {
public:
    ClauseSelector() = default;

    ClauseSelector(const ClauseSelector&) = delete;
    ClauseSelector& operator=(const ClauseSelector&) = delete;

    ~ClauseSelector() override = default;

    std::size_t num_clauses() const noexcept { return clauses_.size(); }

    const std::vector<Clause>& clauses() const noexcept { return clauses_; }

    /// @note Mutable because the propagator moves the clause's watches.
    Clause& clause(ClauseId id);

    const Clause& clause(ClauseId id) const;
    bool is_clause_active(ClauseId id) const;
    void set_clause_active(ClauseId id, bool active);

    const std::vector<std::uint8_t>& active_flags() const noexcept { return clause_active_; }

    /// @brief The next clause to try, or `std::nullopt` once propagation is quiescent.
    virtual std::optional<ClauseId> next() = 0;

    virtual std::optional<ClauseSelection> next_with_hint();

    virtual void add_starting_clause(Clause clause); ///< Takes the next id.

    /// @brief Add a learned asserting clause under the next id; `trail` already
    /// reflects the backjump.
    /// @note `removed_levels` really is the number of decision levels just
    /// *discarded*, not the asserting level and not its complement: after a
    /// restart the solver discards every non-root level regardless of where the
    /// clause asserts, so the two differ.
    virtual void add_asserting_clause(Clause clause, const Trail& trail,
                                      std::size_t removed_levels);

    /// @brief Note an equation learned by propagation, already reduced by the
    /// span it was added to and already on `trail`. Decisions are the same.
    virtual void update_with_propagation(const Equation& reduced_eqn, const Trail& trail);

    virtual void update_with_decision(const Equation& reduced_eqn, const Trail& trail);

    virtual void update_with_decision_clause(std::optional<ClauseId> clause_id);

    /// @brief Note a watch move. The default ignores it; only a watchlist cares.
    void update_with_new_watch(ClauseId id, ClauseRowIndex old_watch, WatchType type,
                               const Trail& trail) override;

    /// @brief Note that every clause was rewritten into a compacted variable
    /// numbering.
    virtual void update_with_compaction(const CompactionTransform& transform, const Trail& trail);

protected:
    /// @brief Store a clause under the next id, with no other side effects.
    virtual void add_clause(Clause clause);

    /// @brief React to a learned equation, propagated or decided alike.
    virtual void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail);

#ifndef NDEBUG
    void debug_assert_database_synchronized() const;
#endif

    std::vector<Clause> clauses_; ///< The clause database; a clause's id is its index.
    std::vector<std::uint8_t> clause_active_; ///< Active flags, parallel to `clauses_`.
};

} // namespace xorcle
