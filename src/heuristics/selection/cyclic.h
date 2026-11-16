#pragma once

/// @file cyclic.h
/// @brief The round-robin clause selector.

#include <cstddef>
#include <optional>
#include <span>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "heuristics/selection/clause_selector.h"

namespace xorcle {

/// @brief Hands out clauses in round-robin order until a full cycle is fruitless.
class Cyclic final : public ClauseSelector {
public:
    explicit Cyclic(const ClauseDatabase& database);

    /// @brief The next active clause in round-robin order, never with a trigger.
    std::optional<ClauseSelection> next() override;

    /// @brief Open a propagation run, capping the attempts it may make; `0` is
    /// uncapped.
    void set_attempt_budget(std::size_t budget);

    /// @brief Whether the last `std::nullopt` from `next()` was the budget rather
    /// than a fixed point.
    bool last_stop_was_budget() const noexcept;

    /// @brief Make a new clause the next one tried.
    void update_with_new_clause(ClauseId id) override;

    /// @brief Try an asserting clause next, but keep the cursor for a replacement.
    void update_with_asserting_clause(ClauseId id, const Trail& trail,
                                      const AssertingClauseInfo& info) override;

    /// @brief Note that an equation was learned, restarting both budgets.
    void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) override;

    /// @brief As `update_with_new_equation()`, and start the next call to `next()` at
    /// @p decision_clause.
    void update_with_decision(const Equation& reduced_eqn, const Trail& trail,
                              std::optional<ClauseId> decision_clause) override;

    /// @brief Move the cursor back into range and onto an active clause.
    void update_with_substitution(std::span<const ClauseId> rewritten_clause_ids,
                                  const IndexSet& dropped_col_idxs, const Trail& trail) override;

private:
    /// @brief End a run, recording whether the budget rather than a fixed point
    /// ended it.
    void stop(bool by_budget);

    ClauseId id_ = 0;
    std::size_t count_ = 0; ///< Clauses handed out since the last equation was learned.

    /// @brief Clauses handed out since the last equation was learned or the run
    /// began, whichever is later.
    std::size_t budget_count_ = 0;
    std::size_t budget_ = 0;
    bool last_stop_was_budget_ = false;

    std::optional<ClauseId> pending_decision_clause_;
};

} // namespace xorcle
