#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "algebra/Equation.h"
#include "algebra/RowSpace.h"
#include "heuristics/Types.h"
#include "search/Trail.h"

namespace heuristics {
using Clause = algebra::Clause;
using Equation = algebra::Equation;
using Trail = search::Trail;

/// @brief Watch variants used by watchlist-based clause selectors.
enum class WatchType {
    Zero, ///< Watch the first equation.
    One,  ///< Watch the second equation.
    Sum   ///< Watch the xor of the two watched equations.
};

/// @brief A clause selected for propagation, optionally with a zero-watch hint.
struct ClauseSelection {
    ClauseId id;
    std::optional<WatchType> zero_watch_hint;
};

/// @brief Available clause-selection strategies.
enum class ClauseSelectorName {
    cyclic,          ///< Simple cyclic iteration.
    lt_watchlist,    ///< Leading-term watchlist.
    exact_watchlist, ///< Exact watchlist with reduced equations.
    exact_watchlist2 ///< Level-indexed exact watchlist with partial reductions.
};

/// @brief Base interface for selecting clauses to attempt propagation.
class ClauseSelector {
protected:
    /// @brief Stored clauses; clause ids are indices into this vector.
    std::vector<Clause> clauses;

    /// @brief Hook for subclasses when a new reduced equation is learned.
    virtual void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) {}

    /// @brief Add a clause to internal storage with no other side effects.
    virtual void add_clause(const Clause& clause) { clauses.push_back(clause); }

public:
    /// @brief Construct an empty selector.
    ClauseSelector();

    /// @brief Virtual destructor for interface use.
    virtual ~ClauseSelector() = default;

    /// @brief Return the number of clauses currently tracked.
    std::size_t get_num_clauses() const;

    /// @brief Return a read-only view of the clause list.
    const std::vector<Clause>& get_clauses() const;

    /// @brief Return a mutable reference to a clause by id.
    Clause& get_clause(ClauseId id);

    /// @brief Return the next clause id to consider, or std::nullopt if none.
    virtual std::optional<ClauseId> next() = 0;

    /// @brief Return the next clause to consider, optionally carrying a zero-watch hint.
    /// Default implementation drops hints and delegates to next().
    virtual std::optional<ClauseSelection> next_with_hint();

    /// @brief Add an initial clause before any propagation or decisions.
    virtual void add_starting_clause(const Clause& clause);

    /// @brief Add a learned asserting clause after backjumping.
    /// @param clause The asserting clause to add.
    /// @param trail Trail after backjumping.
    /// @param jumpable_levels Number of decision levels removed.
    virtual void add_asserting_clause(const Clause& clause, const Trail& trail, std::size_t jumpable_levels);

    /// @brief Notify selector that a propagation occurred.
    /// @param reduced_eqn The reduced equation that was learned.
    /// @param trail Current trail after learning.
    virtual void update_with_propagation(const Equation& reduced_eqn, const Trail& trail);

    /// @brief Notify selector that a decision occurred.
    /// @param reduced_eqn The reduced equation for the decision.
    /// @param trail Current trail after learning.
    virtual void update_with_decision(const Equation& reduced_eqn, const Trail& trail);

    /// @brief Provide an optional clause id hint that should be prioritized.
    virtual void update_with_decision_clause(std::optional<ClauseId> clause_id);

    /// @brief Notify selector that a watch was updated on a clause.
    /// @param id Clause id being updated.
    /// @param old_watch The previously watched equation index.
    /// @param wt Which watch is being updated.
    /// @param trail Current trail.
    virtual void update_with_new_watch(ClauseId id, EquationId old_watch, WatchType wt, const Trail& trail);
};

/// @brief Simple cyclic clause selector.
class Cyclic : public ClauseSelector {
private:
    /// @brief Next clause id to return.
    ClauseId id;

    /// @brief Number of clauses returned since the last full cycle.
    std::size_t count;

    /// @brief Optional restart target based on the last decision clause.
    std::optional<ClauseId> pending_restart_clause;

public:
    /// @brief Construct a cyclic selector.
    explicit Cyclic(std::size_t num_vars);

    /// @brief Return the next clause in cyclic order.
    std::optional<ClauseId> next() override;

    /// @brief Add a clause and restart the cycle from it.
    void add_clause(const Clause& clause) override;

    /// @brief Reset cycle position when a new equation is learned.
    void update_with_new_equation(const Equation& eqn, const Trail& trail) override;

    /// @brief Set a pending restart clause if the id is valid.
    void update_with_decision_clause(std::optional<ClauseId> clause_id) override;
};

/// @brief Watchlist selector keyed by leading terms.
class LTWatchlist : public ClauseSelector {
private:
    /// @brief Map from leading term index to clauses watching that term.
    std::vector<std::vector<ClauseId>> by_leading_term;

    /// @brief Stack of clause ids to process.
    std::vector<ClauseId> stack;

    /// @brief Boolean membership flags for clause ids in the stack.
    std::vector<bool> stack_set;

    /// @brief List of unit clause ids.
    std::vector<ClauseId> unit_clauses;

    /// @brief Push a clause id if it is not already queued.
    void push(ClauseId id);

    /// @brief Push multiple clause ids.
    void append(const std::vector<ClauseId>& ids);

    /// @brief Pop a clause id from the stack.
    ClauseId get_pop();

public:
    /// @brief Construct a watchlist for the given number of variables.
    explicit LTWatchlist(std::size_t num_vars);

    /// @brief Return the next queued clause id.
    std::optional<ClauseId> next() override;

    /// @brief Add a clause and update watchlists.
    void add_clause(const Clause& clause) override;

    /// @brief Add an asserting clause and refresh the queue.
    void add_asserting_clause(const Clause& clause, const Trail& trail, std::size_t jumpable_levels) override;

    /// @brief Queue affected clauses when a new equation is learned.
    void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) override;

    /// @brief Update watchlists when a watched equation changes.
    void update_with_new_watch(ClauseId id, EquationId old_watch, WatchType wt, const Trail& trail) override;
};
} // namespace heuristics
