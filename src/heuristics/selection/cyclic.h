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
/// The default selector, and the simplest thing satisfying the propagation
/// loop's contract: it must eventually report quiescence, and must not report it
/// while a clause could still propagate. `count_` makes both true — it counts
/// clauses handed out since the last equation was learned, so reaching the
/// database size means a whole cycle passed learning nothing, exactly
/// quiescence. Get it wrong and the solver either spins forever or declares
/// quiescence early, and early gives wrong SAT answers, not a crash.
class Cyclic final : public ClauseSelector {
public:
    /// @param num_vars Unused; cyclic order is independent of the variable numbering.
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
    ClauseId id_ = 0; ///< The next clause id to hand out.
    std::size_t count_ = 0; ///< Clauses handed out since the last equation was learned.
    std::optional<ClauseId> pending_restart_clause_; ///< A clause to jump the cursor to.
};

} // namespace xorcle
