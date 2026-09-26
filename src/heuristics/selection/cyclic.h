#pragma once

/// @file cyclic.h
/// @brief The round-robin clause selector.

#include <cstddef>
#include <optional>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/trail.h"
#include "heuristics/selection/clause_selector.h"

namespace xorcle {

/// @brief Hands out clauses in round-robin order until a full cycle is fruitless.
class Cyclic final : public ClauseSelector {
public:
    /// @param num_vars Unused; cyclic order is independent of the variable numbering.
    explicit Cyclic(std::size_t num_vars);

    /// @brief The next active clause in round-robin order, never with a trigger.
    std::optional<ClauseSelection> next() override;

    /// @brief Open a propagation run, capping the attempts it may make; `0` is
    /// uncapped.
    void set_attempt_budget(std::size_t budget);

    /// @brief Whether the last `std::nullopt` from `next()` was the budget rather
    /// than quiescence.
    bool last_stop_was_budget() const noexcept;

    /// @brief Try an asserting clause next, but keep the cursor for a replacement.
    void add_asserting_clause(Clause clause, const Trail& trail,
                              const AssertingClauseInfo& info) override;

    /// @brief Prefer the decision procedure's clause on the next call to `next()`,
    /// if it is active.
    void update_with_decision_clause(std::optional<ClauseId> clause_id);

    /// @brief Move the cursor back into range and onto an active clause.
    void update_with_compaction(const CompactionTransform& transform, const Trail& trail) override;

protected:
    /// @brief Store a clause and make it the next one tried.
    void add_clause(Clause clause) override;

    /// @brief Note that an equation was learned, restarting both budgets.
    void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) override;

private:
    /// @brief End a run, recording whether the budget rather than quiescence
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
