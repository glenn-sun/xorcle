#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/propagation.h"
#include "core/trail.h"
#include "heuristics/compaction/compaction.h"
#include "heuristics/decisions/decision_procedure.h"
#include "heuristics/restarts/restart_policy.h"
#include "heuristics/selection/clause_selector.h"
#include "io/lrup_logger.h"
#include "util/index_set.h"
#include "util/stats.h"

namespace xorcle {

/// @brief When the solver is allowed to compact the variable numbering.
struct CompactionConfig {
    bool enabled = true;

    std::size_t conflict_interval = 16; ///< Conflicts between two successful compactions.
};

struct Solution {
    SolveResult result = SolveResult::unsat;

    /// @brief One bit per *original* variable, present exactly when
    /// `result == SolveResult::sat`. Compaction is already undone: the solver
    /// searches over the compacted variables but reports over the input's.
    std::optional<IndexSet> assignment;
};

/// @brief The counters a run accumulates, named as `RunStats` names them.
struct SolverCounters {
    std::size_t num_attempt_props = 0; ///< Clauses handed to the propagator.
    std::size_t num_watch_updates = 0;
    std::size_t num_propagations = 0;  ///< Equations learned by propagation.
    std::size_t num_conflicts = 0;     ///< Root conflict included.
    std::size_t num_restarts = 0;
};

/// @brief The CDCL(@f$\oplus@f$) solver.
///
/// The loop is propagate, then either analyse a conflict, report SAT, compact,
/// or decide. Everything the algorithm leaves open is delegated: which clause to
/// propagate from to a `ClauseSelector` (which is also the authoritative clause
/// store), which equation to decide to a `DecisionProcedure`, whether to restart
/// to a `RestartPolicy`.
///
/// `num_vars()` never changes: it is the input's variable count, the variable
/// numbering every reported assignment and proof line is written in.
/// The *active* variable count is the numbering the search runs in, which
/// compaction shrinks; the trail, the propagator, the clause database, the decision
/// procedure and the two preallocated equations all live there and are rewritten
/// together whenever it changes.
///
/// @warning Statement order inside the loop is load-bearing beyond correctness.
/// CSIDS in particular is sensitive to the order of
/// `update_with_asserting_clause()` and `update_with_new_clause()`. Reordering
/// any of it changes the search, and therefore the conflict count, without
/// changing the answer.
class Solver {
public:
    /// @brief Construct a solver over an empty clause database. `selector`,
    /// `decisions` and `restarts` are never null (use `NoRestarts` to disable
    /// restarts); `proof_logger` is null for no proof and is not owned, so it must
    /// outlive the solver, and the caller finalizes it, since whether a proof is
    /// worth keeping depends on the result.
    /// @pre `selector`, `decisions` and `restarts` are all non-null.
    /// @pre If `proof_logger` is non-null then `compaction.enabled` is false and
    /// `proof_logger->num_vars() == num_vars`. The CLI enforces the first with
    /// `require_compaction_disabled_for_proof()`.
    Solver(EquationRepr repr, std::size_t num_vars, std::unique_ptr<ClauseSelector> selector,
           std::unique_ptr<DecisionProcedure> decisions, std::unique_ptr<RestartPolicy> restarts,
           LrupLogger* proof_logger = nullptr, const CompactionConfig& compaction = {});

    Solver(const Solver&) = delete;
    Solver& operator=(const Solver&) = delete;

    /// @brief Add a clause from the input instance, in its linear negation.
    /// @pre No solving has happened yet and no compaction has run.
    /// @pre `clause.cols() == num_vars() + 1` and `clause` uses the solver's
    /// representation.
    /// @note The decision procedure is told about the clause *before* the
    /// selector stores it: with no pending learned activity recorded, every
    /// input clause enters at activity zero.
    void add_starting_clause(Clause clause);

    Solution solve();

    std::size_t num_vars() const noexcept { return original_num_vars_; }

    std::size_t num_clauses() const noexcept { return selector_->num_clauses(); }

    std::size_t level() const noexcept { return trail_.level(); }

    const SolverCounters& counters() const noexcept { return counters_; }

    DecisionProcedure::DecisionStats decision_stats() const { return decisions_->decision_stats(); }

private:
    /// @brief Propagate to quiescence; true if the trail now holds
    /// @f$\mathbf{1}@f$.
    ///
    /// @warning The order inside is **learn, then check for conflict**, and it
    /// cannot be swapped. `core/analysis.h`'s resolution step is valid only
    /// because the most recent trail row is @f$\mathbf{1}@f$ at conflict time,
    /// and that is exactly what this ordering establishes: the equation that
    /// closes the conflict is learned first, so it is the row the conflict check
    /// then finds.
    bool propagate_until_conflict();

    /// @brief Analyse, backjump or restart, and learn.
    void handle_non_root_conflict();

    /// @brief Emit the final empty-clause proof line for a root conflict.
    void write_root_unsat_proof();

    void make_decision_step();

    /// @brief Compact if the trigger conditions hold; true if the active variable
    /// numbering changed, in which case the caller must restart the loop rather
    /// than decide.
    bool maybe_compact_before_decision();

    /// @brief Replace every piece of active-numbering state with a prepared plan.
    ///
    /// @warning This discards the root trail for an empty one over the compacted
    /// variables. That is deliberate: the eliminated variables are exactly the
    /// ones the root trail determined, so keeping it would mean carrying rows
    /// that no longer typecheck. It is also what makes root-level units
    /// unrecoverable from the clause database, hence why proofs and compaction
    /// are mutually exclusive.
    void apply_compaction(CompactionPlan plan);

    EquationRepr repr_;

    const std::size_t original_num_vars_;
    std::size_t active_num_vars_;

    /// @brief Active to original variable index. Composed across compactions
    /// rather than replaced, so it always maps the *current* active numbering to
    /// the original one however many steps happened.
    std::vector<std::size_t> active_var_to_original_;

    std::vector<CompactionHistoryEntry> compaction_history_; ///< Oldest first.

    CompactionConfig compaction_;
    bool has_compacted_once_ = false;
    std::size_t conflicts_at_last_compaction_ = 0;

    Trail trail_;
    std::unique_ptr<ClauseSelector> selector_;
    std::unique_ptr<DecisionProcedure> decisions_;
    std::unique_ptr<RestartPolicy> restarts_;
    Propagator propagator_;

    LrupLogger* proof_logger_; ///< Not owned; null for no proof.

    /// @brief The next proof clause id for a line no solver clause backs; the id
    /// space is shared with the clause database, so it tracks that database.
    ClauseId next_proof_clause_id_ = 0;

    Equation conflict_check_eqn_; ///< Scratch for the conflict test.
    Equation false_equation_;     ///< A preallocated @f$\mathbf{1}@f$.

    SolverCounters counters_;
};

} // namespace xorcle
