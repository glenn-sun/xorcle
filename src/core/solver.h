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
#include "heuristics/propagation/attempt_budget.h"
#include "heuristics/restarts/restart_policy.h"
#include "heuristics/selection/clause_selector.h"
#include "io/proof_raw_file.h"
#include "util/index_set.h"
#include "util/progress_reporter.h"
#include "util/random.h"
#include "util/stats.h"

namespace xorcle {

/// @brief When the solver is allowed to compact the variable numbering.
struct CompactionConfig {
    bool enabled = true;

    std::size_t cadence = 16; ///< Conflicts between two successful compactions.
};

/// @brief How far a propagation run may go before deferring to a decision.
struct PropagationConfig {
    /// @brief The fraction of propagated equations the attempt budget steers
    /// toward learning out of order; `0` runs every propagation to quiescence.
    double miss_rate = 0.0;

    /// @brief Learned equations over which the attempt budget falls by a factor
    /// of `e` when none of them is missed.
    std::size_t window = 2000;
};

/// @brief Where the solver reports progress while it runs, and how often.
struct ProgressConfig {
    ProgressReporter* reporter = nullptr; ///< Not owned; null reports nothing.

    std::size_t cadence = 0;    ///< Conflicts between report lines; 0 reports nothing.
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

    /// @brief Asserting clauses, which is what `sum_learned_lbd` averages over:
    /// a root conflict learns none, and a subsumption replacement is not one.
    std::size_t num_asserting_clauses = 0;
    /// @brief Summed trail level and rank *at* the conflicts, before their
    /// backjumps, for the means the progress line reports over an interval.
    std::size_t sum_conflict_level = 0;
    std::size_t sum_conflict_trail_length = 0;

    std::size_t num_minimized_clauses = 0; ///< Learned clauses minimization shrank.
    std::size_t minimize_width_before = 0; ///< Summed width at the asserting point.
    std::size_t minimize_width_after = 0;  ///< Summed width of the clauses as learned.

    /// @brief Summed set bits, constant bit included, over all rows of every learned clause.
    std::size_t learned_equation_bits = 0;

    std::size_t num_deletion_passes = 0;
    std::size_t num_deleted_clauses = 0;
    std::size_t num_demoted_clauses = 0;
    std::size_t num_promoted_clauses = 0; ///< Reasons an improved LBD moved up a tier.

    std::size_t num_subsumed_clauses = 0;      ///< Reason clauses deleted as subsumed.
    std::size_t num_subsumed_conflicting_clauses = 0; ///< Of those, subsumed as the conflicting clause.
    std::size_t num_subsume_replacements = 0;  ///< Subsuming resolvents added as clauses.

    /// @brief Propagation runs a budget stopped short of quiescence.
    std::size_t num_deferred_runs = 0;
    /// @brief Equations learned below the level the solver stood at.
    std::size_t num_out_of_order_learns = 0;
    /// @brief Decisions taken off the trail because a clause a missed
    /// propagation had left unit turned out to forbid them.
    std::size_t num_undone_decisions = 0;
    /// @brief Clauses the decision procedure handed back to propagation rather
    /// than deciding on.
    std::size_t num_decision_propagations = 0;
    /// @brief Of those, the ones that propagated `0 = 1`.
    std::size_t num_decision_conflicts = 0;

    /// @brief Summed budget as a fraction of the clause count, over the runs
    /// that got one, for their mean.
    double sum_budget_fraction = 0.0;
    std::size_t num_budgeted_runs = 0;
    /// @brief That fraction at the last such run.
    double final_budget_fraction = 0.0;
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
           const DeletionConfig& deletion = {}, bool bump_responsible_clauses = false,
           bool subsume = false, const PropagationConfig& propagation = {});

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

    /// @brief Clause ids ever handed out; deletion deactivates rather than
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
    /// @brief What one decision step found.
    enum class DecisionStep {
        decided,
        propagating, ///< `propagating_clause_id_` names a clause to propagate from.
        exhausted    ///< Every active clause is satisfied; any completion is a model.
    };

    /// @brief What one clause's propagation attempt left behind.
    enum class PropagationOutcome {
        nothing,     ///< A watch moved, or the clause turned out to be satisfied.
        propagated,  ///< An equation went on the trail.
        conflicting  ///< That equation was `0 = 1`.
    };

    /// @brief Propagate until quiescence or the attempt budget runs out; true if
    /// the trail now holds `0 = 1`, in which case `conflict_clause_id_` names the
    /// clause that propagated it and `conflict_level_` the level it holds at.
    bool propagate_until_conflict();

    /// @brief Attempts a fruitless stretch gets before the run defers, or `0`
    /// for quiescence; also follows the database and samples the fractions
    /// reported.
    std::size_t attempt_budget();

    /// @brief Try to propagate from one clause, recording whatever it implies at
    /// the level that implies it.
    ///
    /// A clause only the trail's top level falsifies is met by taking that level
    /// off and propagating it again, which is a miss rather than a conflict.
    /// @param trigger_hint Passed through to the propagator.
    /// @param count_learn_as_missed Whether the attempt budget counts what this
    /// learns as missed whatever level it lands at.
    /// @pre `id` names an active clause of the solver's variable count.
    PropagationOutcome propagate_from_clause(ClauseId id,
                                             std::optional<WatchType> trigger_hint = std::nullopt,
                                             bool count_learn_as_missed = false);

    /// @brief Analyse the conflict and learn; a solution once it turns out to
    /// hold at the root.
    /// @pre The trail stands at `conflict_level_`.
    std::optional<Solution> handle_conflict();

    /// @brief Analyse, backjump or restart, and learn.
    void handle_non_root_conflict();

    /// @brief Record the final empty clause for a root conflict.
    void write_root_unsat_proof();

    /// @brief Record the trail row at `trail_idx`, a root unit, as a learned line
    /// unless its one-row reason clause already is that line.
    /// @pre `proof_writer_` is non-null, the trail is not conflicting, the row at
    /// `trail_idx` is at the root and `reason_id` propagated it.
    void write_root_unit(ClauseId reason_id, std::size_t trail_idx);

    /// @brief Record the root units a compaction is about to discard.
    /// @pre The trail still holds them, and `proof_writer_` is non-null.
    void write_compaction_units();

    std::vector<ClauseId> proof_ids(const std::vector<ClauseId>& clause_ids) const;

    /// @brief Discard every decision level above `level`, records included.
    /// @pre `level <= trail_.level()`.
    void backjump_to(std::size_t level);

    DecisionStep make_decision_step();

    /// @brief Fill the variables no clause constrains and answer SAT.
    Solution complete_satisfying_assignment();

    bool reports_progress() const noexcept {
        return progress_.reporter != nullptr && progress_.cadence != 0;
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
    void bump_responsible_clauses(const std::vector<std::size_t>& responsible_trail_idxs);

    /// @brief The clause the decision at this trail row was sampled from, empty
    /// where the fallback literal basis supplied it.
    /// @pre `trail_idx` names a decision row.
    std::optional<ClauseId> decision_clause_at(std::size_t trail_idx) const;

#ifndef NDEBUG
    /// @brief Whether every active clause has an equation that holds under
    /// `model`, a trail span of full rank.
    bool every_active_clause_is_satisfied(const RowSpace& model) const;

    /// @brief Whether the tracked live-clause count still matches the flags it
    /// summarises. Recounting is linear, so this is a debug check only.
    bool active_clause_count_is_consistent() const;
#endif

    /// @brief Delete learned clauses if the conflict interval has elapsed.
    void maybe_delete_clauses();

    /// @brief Demote, deactivate and clear the used flags a prepared plan calls for.
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

    /// @brief The next proof id, which every learned line takes in turn, root
    /// units included.
    ClauseId next_proof_clause_id_ = 0;

    /// @brief Each clause's proof id, indexed by clause id; empty for no proof.
    std::vector<ClauseId> proof_id_of_clause_;

    /// @brief The next proof id when the last compaction ran: the clauses below
    /// it may since have been rewritten.
    ClauseId first_proof_id_of_epoch_ = 0;

    /// @brief The root units since the last compaction over the original
    /// variables, each reduced by the ones before it.
    RowSpace proof_root_span_;
    std::vector<RawUnit> proof_root_units_;   ///< In trail order.
    std::vector<ClauseId> proof_col_idx_to_unit_idx_; ///< Pivot column to the unit's proof id.

    /// @brief A distinct stamp per decision level, reissued whenever a level is
    /// built again after a backjump, so a clause's record of having been
    /// satisfied cannot be read against a different level of the same depth.
    /// Index 0 is the root, whose span only ever grows.
    std::vector<std::size_t> level_stamp_{1};
    std::size_t next_level_stamp_ = 2;

    /// @brief The shallowest level whose span already satisfies @p row.
    ///
    /// Reducing @p row names the span rows it used, and the level filtration
    /// only ever grows, so the deepest level among them is the level the row
    /// first became satisfied at. Recording the shallowest, rather than the
    /// current level, is what lets a record outlive a backjump.
    /// @pre `row` reduces to `0 = 1` against the current span.
    std::size_t shallowest_satisfying_level(const Equation& row);

    /// @brief The deepest level among the trail rows `used_row_idxs` names, which
    /// is `0` when it names none.
    std::size_t level_of_used_rows(const IndexSet& used_row_idxs) const;

    /// @brief Whether the trail falsifies `clause` only at its top level, so that
    /// taking that level off leaves the clause at dimension one.
    /// @pre The trail falsifies `clause` and stands at the level that does.
    bool falsified_only_at_top_level(const Clause& clause) const;

    /// @brief Whether `clause` is known to be satisfied by a level at or below
    /// the current one, so that no attempt on it can find anything.
    bool clause_is_known_satisfied(const Clause& clause) const noexcept {
        return clause.satisfied_stamp != 0 && clause.satisfied_level <= trail_.level() &&
               level_stamp_[clause.satisfied_level] == clause.satisfied_stamp;
    }

    /// @brief The clause whose propagation made the trail conflicting, valid
    /// only between `propagate_until_conflict()` returning true and the analysis
    /// that consumes it.
    ClauseId conflict_clause_id_ = 0;

    /// @brief The level the conflicting `0 = 1` was implied at, which the trail
    /// has been brought down to by the time the analysis reads it.
    std::size_t conflict_level_ = 0;

    /// @brief The clause the last decision step handed back to propagation.
    ClauseId propagating_clause_id_ = 0;

    double propagate_miss_rate_;
    std::size_t propagate_window_;

    /// @brief The attempt budget the miss rate steers, engaged for a nonzero one
    /// and rebuilt when a solve starts.
    std::optional<AttemptBudget> budget_;

    /// @brief Whether the next run ignores the budget, set where full rank has to
    /// be confirmed by a run to quiescence.
    bool force_complete_sweep_ = false;

    /// @brief Whether such a run has already happened on this trail, so that a
    /// second one cannot spin.
    bool did_complete_sweep_ = false;

    /// @brief The trail rows the last propagation's derivation used.
    IndexSet used_row_idxs_;

    /// @brief `&used_row_idxs_` where the level a propagation holds at can differ
    /// from the current one, and null where it cannot.
    IndexSet* tracked_used_row_idxs_ = nullptr;

    /// @brief The raw propagated row plus `0 = 1`, which is what a budgeted run
    /// learns so that the trail reduces it against its own level.
    Equation propagated_row_eqn_;

    Equation false_equation_; ///< A preallocated `0 = 1`.
    Equation satisfied_probe_eqn_; ///< Scratch for `shallowest_satisfying_level()`.

    SolverCounters counters_;

    ProgressConfig progress_;

    /// @brief Whether conflict analysis minimizes the clauses it learns.
    bool minimize_;

    /// @brief The stream each minimization step's representative is drawn from.
    Rng representative_rng_;

    /// @brief Whether a conflict also bumps the clauses at the learned clause's
    /// pivots in the trail basis.
    bool bump_responsible_clauses_;

    /// @brief Whether a learned clause deletes the reason clause it subsumes.
    bool subsume_;

    /// @brief What the last progress line saw, so the next one can report the
    /// interval between them rather than the run so far.
    std::size_t conflicts_at_last_report_ = 0;
    std::size_t sum_level_at_last_report_ = 0;
    std::size_t sum_trail_length_at_last_report_ = 0;
    double time_at_last_report_s_ = 0.0;

    std::chrono::steady_clock::time_point solve_start_;
};

} // namespace xorcle
