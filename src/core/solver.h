#pragma once

#include <chrono>
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
#include "heuristics/deletion/deletion.h"
#include "heuristics/restarts/restart_policy.h"
#include "heuristics/selection/clause_selector.h"
#include "io/proof_raw_file.h"
#include "util/index_set.h"
#include "util/progress_reporter.h"
#include "util/stats.h"

namespace xorcle {

/// @brief When the solver is allowed to compact the variable numbering.
struct CompactionConfig {
    bool enabled = true;

    std::size_t conflict_interval = 16; ///< Conflicts between two successful compactions.
};

/// @brief Where the solver reports progress while it runs, and how often.
struct ProgressConfig {
    ProgressReporter* reporter = nullptr; ///< Not owned; null reports nothing.

    std::size_t conflict_interval = 0;    ///< Conflicts between report lines; 0 reports nothing.
};

struct Solution {
    SolveResult result = SolveResult::unsat;

    /// @brief One bit per *original* variable, with compaction already undone,
    /// present exactly when `result == SolveResult::sat`.
    std::optional<IndexSet> assignment;
};

/// @brief The counters a run accumulates.
struct SolverCounters {
    std::size_t num_attempt_props = 0; ///< Clauses handed to the propagator.
    std::size_t num_watch_updates = 0;
    std::size_t num_propagations = 0;
    std::size_t num_conflicts = 0;     ///< Root conflict included.
    std::size_t num_restarts = 0;
    std::size_t num_reused_levels = 0; ///< Decision levels restarts kept instead of discarding.
    std::size_t num_compactions = 0;

    /// @brief Summed LBD of the clauses learned, for their mean.
    std::size_t sum_learned_lbd = 0;
    /// @brief Summed trail level and rank *at* the conflicts, before their
    /// backjumps, for the means the progress line reports over an interval.
    std::size_t sum_conflict_level = 0;
    std::size_t sum_conflict_rank = 0;

    std::size_t num_minimized_clauses = 0; ///< Learned clauses minimization shrank.
    std::size_t minimize_width_before = 0; ///< Summed width at the asserting point.
    std::size_t minimize_width_after = 0;  ///< Summed width of the clauses as learned.

    std::size_t num_deletion_passes = 0;
    std::size_t num_deleted_clauses = 0;
    std::size_t num_demoted_clauses = 0;
    std::size_t num_promoted_clauses = 0; ///< Reasons an improved LBD moved up a tier.
};

/// @brief The CDCL(⊕) solver.
///
/// The loop is propagate, then either analyse a conflict, report SAT, compact,
/// or decide. The `ClauseSelector` is also the authoritative clause store.
class Solver {
public:
    /// @brief Construct a solver over an empty clause database.
    ///
    /// `proof_writer` is null for no proof and is not owned, so it must outlive
    /// the solver, and the caller finalizes it.
    ///
    /// @pre `selector`, `decisions` and `restarts` are all non-null; use
    /// `NoRestarts` to disable restarts.
    /// @pre If `proof_writer` is non-null then `proof_writer->num_vars() == num_vars`.
    /// @pre If `deletion.key` is `DeletionKey::activity` and deletion is
    /// enabled then `deletion.activity` is non-null.
    Solver(EquationRepr repr, std::size_t num_vars, std::unique_ptr<ClauseSelector> selector,
           std::unique_ptr<DecisionProcedure> decisions, std::unique_ptr<RestartPolicy> restarts,
           RawProofWriter* proof_writer = nullptr, const CompactionConfig& compaction = {},
           const ProgressConfig& progress = {}, bool minimize = true,
           const DeletionConfig& deletion = {}, bool bump_learned_clause_pivots = false);

    Solver(const Solver&) = delete;
    Solver& operator=(const Solver&) = delete;

    /// @brief Add a clause from the input instance, in its linear negation.
    /// @pre No solving has happened yet and no compaction has run.
    /// @pre `clause.cols() == num_vars() + 1` and `clause` uses the solver's
    /// representation.
    /// @note The decision procedure is told about the clause *before* the
    /// selector stores it, so every input clause enters at activity zero.
    void add_starting_clause(Clause clause);

    Solution solve();

    /// @brief Original variable count, before compaction.
    std::size_t num_vars() const noexcept { return original_num_vars_; }

    /// @brief Clause ids ever handed out; deletion tombstones rather than
    /// reclaiming them, so this only grows. `num_active_clauses()` is the
    /// database as it stands.
    std::size_t num_clauses() const noexcept { return selector_->num_clauses(); }

    /// @brief Clauses still in the database: neither deleted nor dropped by a
    /// compaction. Tracked as they change, so a progress line costs nothing.
    std::size_t num_active_clauses() const noexcept { return num_active_clauses_; }

    /// @brief Variables left after compaction, which is `num_vars()` until one runs.
    std::size_t active_num_vars() const noexcept { return active_num_vars_; }

    std::size_t level() const noexcept { return trail_.level(); }

    const SolverCounters& counters() const noexcept { return counters_; }

    DecisionProcedure::DecisionStats decision_stats() const { return decisions_->decision_stats(); }

    RestartStats restart_stats() const { return restarts_->stats(); }

private:
    /// @brief Propagate to quiescence; true if the trail now holds `0 = 1`.
    bool propagate_until_conflict();

    /// @brief Analyse, backjump or restart, and learn.
    void handle_non_root_conflict();

    /// @brief Record the final empty clause for a root conflict.
    void write_root_unsat_proof();

    /// @brief Record the root trail a compaction is about to discard.
    /// @pre The trail still holds it, and `proof_writer_` is non-null.
    void write_compaction_units();

    /// @brief Discard every decision level above `level`, records included.
    /// @pre `level <= trail_.level()`.
    void backjump_to(std::size_t level);

    void make_decision_step();

    bool reports_progress() const noexcept {
        return progress_.reporter != nullptr && progress_.conflict_interval != 0;
    }

    /// @brief Report progress if the conflict interval has elapsed since the last
    /// line.
    void maybe_report();

    /// @brief Write one progress line and open a fresh interval.
    void report_progress();

    /// @brief The state one line reports, with the averages and the rate taken
    /// over the interval since the last one.
    ProgressSnapshot progress_snapshot() const;

    /// @brief Compact if the trigger conditions hold; true if the active variable
    /// numbering changed, in which case the caller must restart the loop rather
    /// than decide.
    bool maybe_compact_before_decision();

    /// @brief Replace every piece of active-numbering state with a prepared plan.
    ///
    /// @warning This discards the root trail, so root-level units become
    /// unrecoverable from the clause database.
    void apply_compaction(CompactionPlan plan);

    /// @brief Mark a resolved reason used, and promote it if this conflict
    /// measured a better LBD than the stored one.
    void note_reason_use(ClauseId id, std::size_t lbd);

    /// @brief Bump the clause owning each of a learned clause's pivots: the
    /// reason of that trail row, or the clause its decision was sampled from.
    void bump_pivot_clauses(const std::vector<std::size_t>& pivot_trail_idxs);

    /// @brief The clause the decision at this trail row was sampled from, empty
    /// where the fallback literal basis supplied it.
    /// @pre `trail_idx` names a decision row.
    std::optional<ClauseId> decision_clause_at(std::size_t trail_idx) const;

#ifndef NDEBUG
    /// @brief Whether the tracked live-clause count still matches the flags it
    /// summarises. Recounting is linear, so this is a debug check only.
    bool active_clause_count_is_consistent() const;
#endif

    /// @brief Delete learned clauses if the conflict interval has elapsed.
    void maybe_delete_clauses();

    /// @brief Demote, tombstone and clear the used flags a prepared plan calls for.
    void apply_deletion(const DeletionPlan& plan);

    EquationRepr repr_;

    const std::size_t original_num_vars_;
    std::size_t active_num_vars_;

    /// @brief Active to original variable index, composed across compactions.
    std::vector<std::size_t> active_var_to_original_;

    std::vector<CompactionHistoryEntry> compaction_history_; ///< Oldest first.

    CompactionConfig compaction_;
    DeletionConfig deletion_;
    bool has_compacted_once_ = false;
#ifndef NDEBUG
    bool has_solved_ = false;
#endif
    std::size_t conflicts_at_last_compaction_ = 0;
    std::size_t conflicts_at_last_deletion_ = 0;
    std::size_t num_active_clauses_ = 0;

    Trail trail_;

    /// @brief The clause each decision level's decision came from, indexed by
    /// level less one, and empty where the fallback basis supplied it.
    std::vector<std::optional<ClauseId>> decision_clause_of_level_;

    std::unique_ptr<ClauseSelector> selector_;
    std::unique_ptr<DecisionProcedure> decisions_;
    std::unique_ptr<RestartPolicy> restarts_;
    Propagator propagator_;

    RawProofWriter* proof_writer_; ///< Not owned; null for no proof.

    /// @brief The next proof clause id for a line no solver clause backs, from
    /// the id space shared with the clause database.
    ClauseId next_proof_clause_id_ = 0;

    Equation conflict_check_eqn_;
    Equation false_equation_; ///< A preallocated `0 = 1`.

    SolverCounters counters_;

    ProgressConfig progress_;

    /// @brief Whether conflict analysis minimizes the clauses it learns.
    bool minimize_;

    /// @brief Whether a conflict also bumps the clauses at the learned clause's
    /// pivots in the trail basis.
    bool bump_learned_clause_pivots_;

    /// @brief What the last progress line saw, so the next one can report the
    /// interval between them rather than the run so far.
    std::size_t conflicts_at_last_report_ = 0;
    std::size_t sum_level_at_last_report_ = 0;
    std::size_t sum_rank_at_last_report_ = 0;
    double time_at_last_report_s_ = 0.0;

    std::chrono::steady_clock::time_point solve_start_;
};

} // namespace xorcle
