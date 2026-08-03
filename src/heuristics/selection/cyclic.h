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
///
/// The default selector, and the simplest thing
/// that satisfies the propagation loop's contract: it must eventually report
/// quiescence, and it must not report it while a clause could still propagate.
///
/// `count_` is what makes both true. It counts clauses handed out since the last
/// equation was learned, and reaching the database size means a whole cycle
/// passed without anything being learned — which is exactly quiescence. Every
/// learned equation resets it. Getting this wrong makes the solver either spin
/// forever or declare quiescence early, and declaring it early produces wrong
/// SAT answers rather than a crash.
class Cyclic final : public ClauseSelector {
public:
    /// @brief Construct a cyclic selector.
    /// @param num_vars Number of active variable columns. Unused; the cyclic
    /// order does not depend on the variable space, but the selector interface
    /// is constructed uniformly.
    explicit Cyclic(std::size_t num_vars);

    /// @brief The next active clause in cyclic order, or `std::nullopt` at quiescence.
    std::optional<ClauseId> next() override;

    /// @brief Note that an equation was learned, restarting the cycle budget.
    void update_with_new_equation(const Equation& reduced_eqn, const Trail& trail) override;

    /// @brief Prefer the decision procedure's clause on the next call to `next()`.
    void update_with_decision_clause(std::optional<ClauseId> clause_id) override;

    /// @brief Move the cursor back into range and onto an active clause.
    void update_with_compaction(const CompactionTransform& transform, const Trail& trail) override;

protected:
    /// @brief Store a clause and make it the next one tried.
    void add_clause(Clause clause) override;

private:
    /// @brief The next clause id to hand out.
    ClauseId id_ = 0;

    /// @brief Clauses handed out since the last equation was learned.
    std::size_t count_ = 0;

    /// @brief A clause to jump the cursor to, from the decision procedure.
    std::optional<ClauseId> pending_restart_clause_;
};

} // namespace xorcle
