#pragma once

/// @file cyclic.h
/// @brief The round-robin clause selector, and the default.

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
    ClauseId id_ = 0;
    std::size_t count_ = 0; ///< Clauses handed out since the last equation was learned.
    std::optional<ClauseId> pending_restart_clause_;
};

} // namespace xorcle
