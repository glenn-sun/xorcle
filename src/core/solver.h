#pragma once

/// @file solver.h
/// @brief The CDCL(@f$\oplus@f$) search loop: the paper's Algorithm 1.

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "algebra/algebra.h"
#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/propagation.h"
#include "core/trail.h"
#include "heuristics/compaction/compaction.h"
#include "heuristics/decisions/decision_procedure.h"
#include "heuristics/restarts/restart_policy.h"
#include "heuristics/selection/clause_selector.h"
#include "io/lrup_logger.h"
#include "util/bitmap.h"
#include "util/stats.h"

namespace xorcle {

/// @brief When the solver is allowed to compact the variable space.
struct CompactionConfig {
    /// @brief Whether to compact at all. On by default.
    bool enabled = true;

    /// @brief Conflicts that must elapse between two successful compactions.
    std::size_t conflict_interval = 16;
};

/// @brief What a solve produced.
struct Solution {
    /// @brief Whether the instance was proved unsatisfiable or satisfied.
    SolveResult result = SolveResult::unsat;

    /// @brief The satisfying assignment, one bit per *original* variable.
    ///
    /// Present exactly when `result == SolveResult::sat`. Compaction is already
    /// undone: the solver searches over the compacted variables but reports over
    /// the ones the input named.
    std::optional<Bitmap> assignment;
};

/// @brief The counters a run accumulates, named as `RunStats` names them.
struct SolverCounters {
    std::size_t num_attempt_props = 0; ///< Clauses handed to the propagation engine.
    std::size_t num_watch_updates = 0; ///< Watches moved.
    std::size_t num_propagations = 0;  ///< Equations learned by propagation.
    std::size_t num_conflicts = 0;     ///< Conflicts reached, root conflict included.
    std::size_t num_restarts = 0;      ///< Restarts performed.
};

/// @brief The CDCL(@f$\oplus@f$) solver: Algorithm 1.
///
/// The loop is propagate, then either analyse a conflict, report SAT, compact,
/// or decide. Everything the algorithm leaves open is delegated: which clause to
/// propagate from to a `ClauseSelector`, which equation to decide to a
/// `DecisionProcedure`, whether to restart to a `RestartPolicy`.
/// This class performs those steps; it does not
/// choose them.
///
/// ## The active variable space
///
/// `num_vars()` never changes: it is the input's variable count and the space
/// every reported assignment and every proof line is written in.
/// `active_num_vars()` is the space the search actually runs in, which
/// compaction shrinks. The trail, the propagation engine, the clause database,
/// the decision procedure and the two preallocated equations all live in the
/// active space and are rewritten together whenever it changes.
///
/// @note The solver keeps no clause vector of its own; the `ClauseSelector` is
/// the authoritative store, and compaction writes rewritten clauses back through
/// it.
///
/// @warning Statement order inside the loop is load-bearing beyond correctness.
/// The decision procedure and the selector are notified at points the 0.1 tree
/// fixed, and CSIDS in particular is sensitive to the order of
/// `update_with_asserting_clause()` and `update_with_new_clause()`. Reordering
/// any of it changes the search — and therefore the conflict count the
/// differential harness compares — without changing the answer.
class Solver {
public:
    /// @brief Construct a solver over an empty clause database.
    /// @param backend Storage representation for every equation the solver builds.
    /// @param num_vars Number of variables in the input instance.
    /// @param selector Clause selector, and the clause database. Never null.
    /// @param decisions Decision procedure. Never null.
    /// @param restarts Restart policy. Never null; use `NoRestarts` to disable.
    /// @param proof_logger Proof writer, or null for no proof. Not owned, and it
    /// must outlive the solver. The caller finalizes it, because whether a proof
    /// is worth keeping depends on the result.
    /// @param compaction Compaction settings.
    /// @pre If `proof_logger` is non-null then `compaction.enabled` is false and
    /// `proof_logger->num_vars() == num_vars`. The CLI enforces the first with
    /// `require_compaction_disabled_for_proof()`.
    Solver(Algebra backend, std::size_t num_vars, std::unique_ptr<ClauseSelector> selector,
           std::unique_ptr<DecisionProcedure> decisions, std::unique_ptr<RestartPolicy> restarts,
           LrupLogger* proof_logger = nullptr, const CompactionConfig& compaction = {});

    Solver(const Solver&) = delete;
    Solver& operator=(const Solver&) = delete;

    /// @brief Add a clause from the input instance.
    /// @param clause The clause, in its linear negation, over `num_vars()` variables.
    /// @pre No solving has happened yet and no compaction has run.
    /// @note The decision procedure is told about the clause *before* the
    /// selector stores it, matching the 0.1 tree: with no pending learned
    /// activity recorded, every input clause enters at activity zero.
    void add_starting_clause(Clause clause);

    /// @brief Run the search to completion.
    /// @return UNSAT, or SAT with an assignment over the original variables.
    Solution solve();

    /// @brief Number of variables in the input instance.
    std::size_t num_vars() const noexcept { return original_num_vars_; }

    /// @brief Number of variables in the current, possibly compacted, active space.
    std::size_t active_num_vars() const noexcept { return active_num_vars_; }

    /// @brief Number of clauses currently in the database.
    std::size_t num_clauses() const noexcept { return selector_->num_clauses(); }

    /// @brief The current decision level.
    std::size_t level() const noexcept { return trail_.level(); }

    /// @brief The run counters.
    const SolverCounters& counters() const noexcept { return counters_; }

    /// @brief The decision procedure's own statistics.
    DecisionProcedure::DecisionStats decision_stats() const { return decisions_->decision_stats(); }

private:
    /// @brief Propagate until nothing more propagates or the trail conflicts.
    /// @return True if the trail now contains @f$\mathbf{1}@f$.
    ///
    /// @warning The order inside is **learn, then check for conflict**, and it
    /// cannot be swapped. `core/analysis.h`'s resolution step is valid only
    /// because the most recent trail row is @f$\mathbf{1}@f$ at conflict time,
    /// and that is exactly what this ordering establishes: the equation that
    /// closes the conflict is learned first, so it is the row the conflict check
    /// then finds.
    bool propagate_until_conflict();

    /// @brief Analyse a conflict above the root, backjump or restart, and learn.
    void handle_non_root_conflict();

    /// @brief Emit the final empty-clause proof line for a root conflict.
    void write_root_unsat_proof();

    /// @brief Decide the next equation and apply it to the trail.
    void make_decision_step();

    /// @brief Compact the variable space if the trigger conditions hold.
    /// @return True if the active space changed, in which case the caller must
    /// restart the loop rather than decide.
    bool maybe_compact_before_decision();

    /// @brief Replace every piece of active-space state with a prepared plan.
    /// @param plan The compaction to commit.
    ///
    /// @warning This discards the root trail, replacing it with an empty one over
    /// the compacted variables. That is deliberate: the eliminated variables are
    /// exactly the ones the root trail determined, so keeping it would mean
    /// carrying rows that no longer typecheck. It is also what makes root-level
    /// units unrecoverable from the clause database, hence why proofs and
    /// compaction are mutually exclusive in 0.2.
    void apply_compaction(CompactionPlan plan);

    Algebra backend_;

    /// @brief Number of variables in the input instance; never changes.
    const std::size_t original_num_vars_;

    /// @brief Number of variables in the space the search currently runs in.
    std::size_t active_num_vars_;

    /// @brief Active variable index to original variable index.
    ///
    /// Composed across compactions rather than replaced, so it always maps the
    /// *current* active space to the original one however many steps happened.
    std::vector<std::size_t> active_var_to_original_;

    /// @brief Every compaction applied so far, oldest first.
    std::vector<CompactionHistoryEntry> compaction_history_;

    CompactionConfig compaction_;
    bool has_compacted_once_ = false;
    std::size_t conflicts_at_last_compaction_ = 0;

    Trail trail_;
    std::unique_ptr<ClauseSelector> selector_;
    std::unique_ptr<DecisionProcedure> decisions_;
    std::unique_ptr<RestartPolicy> restarts_;
    PropagationEngine propagation_;

    /// @brief Proof writer, or null. Not owned.
    LrupLogger* proof_logger_;

    /// @brief The next proof clause id for a line no solver clause backs.
    ///
    /// Only the final empty clause needs one in 0.2, but the id space is shared
    /// with the clause database, so it is tracked as the database grows.
    ClauseId next_proof_clause_id_ = 0;

    /// @brief Preallocated scratch for the conflict test, refilled per propagation.
    Equation conflict_check_eqn_;

    /// @brief A preallocated @f$\mathbf{1}@f$ over the active variables.
    Equation false_equation_;

    SolverCounters counters_;
};

} // namespace xorcle
