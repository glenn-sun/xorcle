#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_database.h"
#include "core/clause_id.h"
#include "core/phase_timer.h"
#include "core/propagation.h"
#include "core/saved_trail.h"
#include "core/trail.h"
#include "heuristics/bve/bve.h"
#include "heuristics/compaction/compaction.h"
#include "heuristics/decisions/csids.h"
#include "heuristics/deletion/deletion.h"
#include "heuristics/equivalent_clauses/equivalent_clause_table.h"
#include "heuristics/implication_graph/implication_graph.h"
#include "heuristics/propagation/attempt_budget.h"
#include "heuristics/restarts/lbd_restarts.h"
#include "heuristics/restarts/restart_policy.h"
#include "heuristics/restarts/stabilize.h"
#include "heuristics/selection/clause_selector.h"
#include "heuristics/selection/cyclic.h"
#include "io/proof_writer.h"
#include "util/index_set.h"
#include "util/progress_reporter.h"
#include "util/random.h"
#include "util/stats.h"

namespace xorcle {

/// @brief The trail an LRUP(⊕) checker builds while it reads one proof line.
class HintTrail;

/// @brief The attempt budget, under which a propagation run may defer to a decision.
struct PropagationConfig {
    /// @brief The fraction of propagated equations the attempt budget steers
    /// toward learning out of order.
    double miss_rate;

    /// @brief Learned equations over which the attempt budget falls by a factor
    /// of `e` when none of them is missed.
    std::size_t window;
};

/// @brief When and how vivification narrows clauses.
///
/// A pass walks the clauses of width 3 and up in one uniformly random order and tries
/// a clause of width `w` with probability `2^(3 - w)`; a clause it tried is left out of
/// later passes unless it is new or changed width since, and one it skipped is not.
struct VivifyConfig {
    /// @brief The random bases a pass tries on a clause, each on the clause as the last left it.
    std::size_t rounds;

    std::size_t cadence; ///< Conflicts after a pass before the next may run.

    /// @brief The rows a pass may assume and imply, per row the search decided or
    /// propagated since the last pass.
    double effort;
};

/// @brief A clause an order narrowed to, with the proof lines that derive it.
struct VivifyNarrowing {
    Clause clause;
    /// @brief In replay order; empty without a proof.
    std::vector<ClauseId> proof_hints;
};

/// @brief When the subsumption pass runs.
///
/// A pass at a root visit checks pairs of active clauses of which one was added or
/// changed since the last pass: a subsumed wider clause D is deleted, and a
/// strengthened D loses a row. It visits every clause as the narrower clause,
/// narrowest first, then each unchanged clause as the wider clause, until its
/// budget is spent. A pass due above the root forces a restart with replay.
struct SubsumePassConfig {
    std::size_t cadence; ///< Conflicts after a pass before the next may run.

    /// @brief The rows a pass may read in its checks per row the search decided or
    /// implied since the last pass.
    double effort;
};

/// @brief Where the solver reports progress while it runs, and how often.
struct ProgressConfig {
    ConsoleProgressReporter* reporter; ///< Not owned; null reports nothing.

    std::size_t cadence; ///< Conflicts between report lines; 0 reports nothing.
};

/// @brief How the solver is configured; a feature that is off is absent.
struct SolverConfig {
    bool renumber; ///< Whether a substitution may be followed by a renumbering.
    bool minimize;
    bool bump_responsible_clauses;
    bool otf_subsume;
    /// @brief The window of the average of the LBDs measured at a clause's uses as a reason;
    /// the newest measurement weighs its reciprocal.
    std::size_t lbd_window;
    std::optional<PropagationConfig> attempt_budget;
    std::optional<DeletionConfig> deletion;
    std::optional<ImplicationGraphConfig> implication_graph;
    std::optional<VivifyConfig> vivify;
    std::optional<BveConfig> bve;
    std::optional<SubsumePassConfig> inproc_subsume_strengthen;
    std::optional<StabilizeConfig> stabilize;
    ProgressConfig progress;
};

struct Solution {
    SolveResult result = SolveResult::unsat;

    /// @brief One bit per *original* variable, with compaction already undone,
    /// present exactly when `result == SolveResult::sat`.
    std::optional<IndexSet> assignment;
};

/// @brief The CDCL(⊕) solver.
///
/// The loop is propagate, then either analyse a conflict, report SAT, compact,
/// run the implication graph, or decide.
class Solver {
public:
    /// @brief Construct a solver over the variables `used_vars` of `num_vars`, with
    /// an empty clause database.
    ///
    /// Clauses, the selector and `decisions` are over `used_vars.size()` variables, bit
    /// `i` being variable `used_vars[i]`. Models and proofs are over `num_vars`, the
    /// unused variables false in a model.
    ///
    /// `proof_writer` is null for no proof and is not owned, so it must outlive
    /// the solver, and the caller finalizes it.
    ///
    /// @pre `used_vars` is strictly ascending and below `num_vars`.
    /// @param make_selector Builds the clause selector over the solver's database, once.
    /// @pre `make_selector` returns non-null, and `decisions` and `restarts` are
    /// non-null; use `NoRestarts` to disable restarts.
    /// @pre An attempt budget comes with a `Cyclic` selector.
    /// @pre If `proof_writer` is non-null then `proof_writer->num_vars() == num_vars`.
    Solver(EquationRepr repr, std::size_t num_vars, std::vector<std::size_t> used_vars,
           const ClauseSelectorFactory& make_selector, std::unique_ptr<Csids> decisions,
           std::unique_ptr<RestartPolicy> restarts, ProofWriter* proof_writer,
           const SolverConfig& config);

    Solver(const Solver&) = delete;
    Solver& operator=(const Solver&) = delete;

    /// @brief Add a clause from the input instance, in its linear negation.
    ///
    /// A clause with no rows is the empty clause: it stays out of the database,
    /// and `solve()` answers UNSAT citing it. A tautology, whose linear negation
    /// holds `0 = 1`, stays out too.
    /// @pre No solving has happened yet and no compaction has run.
    /// @pre `clause` is over the used variables and uses the solver's
    /// representation.
    /// @note The decision procedure is told about the clause *before* the
    /// database stores it, so every input clause enters at activity zero.
    void add_starting_clause(Clause clause);

    Solution solve();

    /// @brief Original variable count, the unused variables included.
    std::size_t num_vars() const noexcept { return original_num_vars_; }

    /// @brief Clause ids ever handed out, which only grows: deletion deactivates
    /// rather than reclaiming them.
    std::size_t num_clauses() const noexcept { return clause_database_.num_clauses(); }

    /// @brief Clauses still in the database: neither deleted nor dropped by a
    /// compaction.
    std::size_t num_active_clauses() const noexcept { return num_active_clauses_; }

    /// @brief Used variables left after compaction.
    std::size_t active_num_vars() const noexcept { return active_num_vars_ - num_dropped_vars_; }

    std::size_t level() const noexcept { return trail_.level(); }

    const ClauseDatabase& clause_database() const noexcept { return clause_database_; }

    /// @brief The solver's counters so far, the instance's and its heuristics' included.
    RunStats stats() const;

private:
    /// @brief What one decision step found.
    enum class DecisionStep {
        decided,
        propagating, ///< `propagating_clause_id_` names a clause to propagate from.
        conflicting, ///< A replayed reason propagated `0 = 1`, as `conflict_clause_id_` names.
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

    /// @brief Whether the last propagation run ended on its attempt budget.
    bool last_run_stopped_on_budget() const noexcept {
        return cyclic_selector_ != nullptr && cyclic_selector_->last_stop_was_budget();
    }

    /// @brief Attempts a fruitless stretch gets before the run defers, or `0`
    /// for quiescence; also follows the database and samples the fractions
    /// reported.
    std::size_t attempt_budget();

    /// @brief Try to propagate from one clause, recording whatever it implies at
    /// the level that implies it.
    ///
    /// A clause only the trail's top level falsifies is met by taking that level
    /// off and propagating it again, which is a miss rather than a conflict; in a
    /// vivification try, any conflict goes in at the current level, and no level is
    /// taken off.
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

    /// @brief The lines of the root units at the rows @p used of `proof_root_span_`, in
    /// row order.
    std::vector<ClauseId> root_unit_lines(const IndexSet& used) const;

    /// @brief Write the line of a clause a substitution rewrites, citing the root units
    /// it used and then the clause's line @p line.
    /// @return The rewritten clause's line.
    /// @pre `proof_writer_` is non-null, and the root trail still holds the units.
    ClauseId write_substitution_line(const Clause& clause, const Clause& rewritten, ClauseId line);

    /// @brief Record every equation one run of the implication graph proved.
    /// @return The line of each unit the run is to feed, in the run's order.
    /// @pre `proof_writer_` is non-null and the trail is at the root.
    std::vector<ClauseId> write_implication_graph_lines(
        const ImplicationGraphProofRecord& record);

    /// @brief The lines a replay cited, in order, and whether the last of them
    /// falsified a clause.
    struct Citations {
        std::vector<ClauseId> hints;
        bool conflicted = false;
    };

    /// @brief Cite the clause behind each arc in turn, stopping at the one the
    /// trail falsifies.
    Citations replay_arcs(std::span<const ClauseId> arcs, HintTrail& trail);

    /// @brief The rows of clause @p id over the original variables.
    std::vector<Equation> lift_clause(ClauseId id) const;

    std::vector<ClauseId> proof_ids(const std::vector<ClauseId>& clause_ids) const;

    /// @brief Discard every decision level above `level`, records included.
    /// @pre `level <= trail_.level()`.
    void backjump_to(std::size_t level);

    DecisionStep make_decision_step();

    /// @brief Decide the next saved decision the trail leaves open and attempt the
    /// saved reasons of its level in order, or as `make_decision_step()` once no
    /// saved level is left.
    DecisionStep replay_decision_step();

    /// @brief What one step of a visit to the root did.
    enum class VisitStep {
        done,      ///< Nothing is left to do at this root.
        again,     ///< The root changed, so propagate before the next step.
        unsat      ///< BVE derived the empty clause.
    };

    /// @brief One step of a visit to the quiescent root: substitute the root units,
    /// else run BVE or the next due technique in the order the subsumption pass,
    /// vivification, the implication graph.
    /// @pre The trail is at the root and quiescent.
    VisitStep visit_root_step();

    /// @brief Whether a technique that forces a restart is due.
    bool forced_restart_is_due();

    /// @brief Save each level's decision and its reasons for the replay, and backjump
    /// to the root.
    /// @pre The trail is above the root and quiescent.
    void force_restart();

    /// @brief Note that @p deleted_id leaves the database with @p kept_id in its place,
    /// so that a replay attempts the kept clause for it.
    void note_clause_kept_in_place_of(ClauseId deleted_id, ClauseId kept_id);

    /// @brief End the replay, if one is under way, adding its time.
    void end_replay();

    /// @brief The equation the clause @p id implies modulo the trail, `0 = 1` where it
    /// is falsified; null where it is satisfied or has two dimensions left.
    /// @return Scratch valid until the next call.
    const Equation* implied_on_trail(ClauseId id);

    /// @brief Put @p implied_eqn on the trail at its top level with clause @p id as
    /// its reason, moving no watch.
    /// @pre No attempt budget, and the trail leaves clause @p id implying exactly
    /// @p implied_eqn, partially reduced.
    PropagationOutcome learn_implied(ClauseId id, const Equation& implied_eqn);

    /// @brief The search `solve()` times.
    Solution search();

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

    /// @brief Substitute the root units, and renumber if that frees enough words.
    /// @pre The trail is at the root, quiescent, and has rows.
    void substitute_root_units();

    /// @brief Rewrite the clauses by a prepared plan and drop the variables the
    /// root trail fixes.
    ///
    /// @warning This discards the root trail, so root-level units become
    /// unrecoverable from the clause database.
    void apply_substitution(SubstitutionPlan plan);

    /// @brief Renumber the variables without the dropped ones if `--compact` is on and that frees a word.
    void maybe_renumber();

    /// @brief Renumber the variables without the dropped ones.
    /// @pre `apply_substitution()` has just run.
    void apply_renumbering();

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
    /// summarises.
    bool active_clause_count_is_consistent() const;

    /// @brief Whether no active clause is unit or falsified modulo the trail, which
    /// propagation to a fixed point leaves; on a large database only every so
    /// many calls check, and the rest return true.
    bool debug_nothing_left_to_propagate();

    /// @brief Whether the equation at @p trail_idx plus `0 = 1` lies in the span of clause
    /// @p id's rows and the other trail rows at its level or below, as `Trail::learn()`
    /// requires of a reason; checked every so many calls on a long trail.
    bool debug_reason_implies(ClauseId id, std::size_t trail_idx);
#endif

    /// @brief Delete learned clauses if the conflict interval has elapsed.
    void maybe_delete_clauses();

    /// @brief Whether a run of the implication graph is due: before the first run,
    /// and once the conflict interval has elapsed since the last run or skip; a due
    /// run with fewer new width-2 clauses than the skip threshold is skipped here,
    /// and is not due.
    bool implication_graph_is_due();

    /// @brief Count a point a run was due at, skipped or not.
    void count_implication_graph_point();

    /// @brief Read the implication graph and add the units it finds to the root.
    /// @pre The trail is at the root, quiescent and empty.
    void run_implication_graph();

    /// @brief Run a BVE pass over the clause database and drop the variables it
    /// takes out of every clause; a pass eliminating nothing changes nothing.
    ///
    /// Another pass is due if this one removed at least `success_rate` of the clauses
    /// it read. A unit among the resolvents leaves the renumbering to the substitution
    /// it brings.
    /// @return False if a resolvent was the empty clause.
    /// @pre The trail is empty and no clause has been learned.
    bool run_bve();

    /// @brief Record each resolvent of a BVE pass as a line citing its two
    /// parents, ending at one that is the empty clause.
    /// @return The line of each resolvent written, in the plan's order.
    /// @pre `proof_writer_` is non-null and the clause store is as the plan read it.
    std::vector<ClauseId> write_bve_lines(const BvePlan& plan);

    /// @brief Add @p unit as a width-1 clause, so propagation learns it the way
    /// it learns a unit clause conflict analysis produced.
    /// @param proof_line The line proving the clause, read only under a proof.
    /// @pre The trail is at the root and `unit` is not in its span.
    void feed_unit(const Equation& unit, ClauseId proof_line);

    /// @brief Whether a vivification pass is due: the cadence since the last, with a
    /// conflict unless none has run, and a clause of width 3 or more to try.
    bool vivify_is_due() const;

    /// @brief Run a vivification pass.
    /// @pre The trail is at the root and quiescent.
    void vivify_pass();

    /// @brief The active clauses at least `vivify_min_width` wide that are the reason
    /// of no trail row and that no pass tried since they were added or changed width.
    std::vector<ClauseId> vivify_candidates();

    /// @brief Assume the rows of one uniformly random basis of clause @p id in
    /// turn, propagating after each, and undo them.
    /// @return A narrowing of the clause that the formula implies, if the order found one.
    /// @pre The trail is at the root and quiescent, and the clause is active, at
    /// least two rows wide and the reason of no trail row.
    std::optional<VivifyNarrowing> vivify_one_order(ClauseId id);

    /// @brief The proof lines that derive @p narrowed by unit propagation: the
    /// root units and the reasons of the trail rows up to @p last_level that add
    /// to what its linear negation and the lines before them imply, through the
    /// first that conflicts.
    /// @param falsified_id A clause the trail falsifies though no row of the trail
    /// records the conflict, replayed after the trail rows.
    /// @pre The trail up to @p last_level, with @p narrowed's linear negation,
    /// propagates to a conflict, or falsifies @p falsified_id.
    std::vector<ClauseId> vivify_proof_hints(const Clause& narrowed, std::size_t last_level,
                                             std::optional<ClauseId> falsified_id) const;

    /// @brief Open a decision level with @p reduced_eqn, drawn from @p decision_clause
    /// if any.
    /// @pre `reduced_eqn` is reduced by the trail span and leads.
    void open_level(const Equation& reduced_eqn, std::optional<ClauseId> decision_clause);

    /// @brief Overwrite clause @p id with @p narrowing, hand the clause's deletion
    /// state to a kept clause with its span instead, or keep it, with its proof
    /// line, in @p units if it is one row.
    /// @return Whether the clause goes on under its id.
    /// @pre `narrowing.clause` is a narrowing of clause @p id: its rows lie in the span
    /// of that clause's rows and the root rows, so that it alone implies the clause.
    bool vivify_replace(ClauseId id, VivifyNarrowing narrowing,
                        std::vector<std::pair<Equation, ClauseId>>& units);

    /// @brief Overwrite clause @p id with @p narrowed at the root, or hand its
    /// deletion state to a kept clause with that span and delete it.
    /// @return Whether the clause goes on under its id.
    /// @pre @p narrowed implies the clause, is in reduced row-echelon form and has
    /// at least two rows; @p proof_hints derive it under a proof.
    bool put_narrowing_in_place(ClauseId id, Clause narrowed,
                                const std::vector<ClauseId>& proof_hints);

    /// @brief Whether a subsumption pass is due: the cadence since the last, with a
    /// conflict unless none has run.
    bool subsume_pass_is_due() const;

    /// @brief Run one subsumption pass.
    /// @pre The trail is at the root and quiescent.
    void run_subsume_pass();

    /// @brief Apply a prepared plan and clear the used flags.
    void apply_deletion(const DeletionPlan& plan);

    /// @brief The LBD and deletion state of a clause, as its equivalent clause takes them over.
    struct EvictedClauseState {
        bool is_learned;
        std::optional<double> lbd; ///< Unset while unmeasured.
        std::size_t unused_passes;
        bool is_used_since_deletion;
    };

    EvictedClauseState evicted_clause_state(ClauseId id) const;

    /// @brief The rows the search has put on the trail: its decisions and propagations.
    std::size_t search_rows() const {
        return decisions_->counters().decisions + propagation_counters_.num_propagations;
    }

    /// @brief Write a learned line proving @p clause from @p hints; its id, or `0` without
    /// a proof.
    ClauseId write_proof_line(const Clause& clause, const std::vector<ClauseId>& hints);

    /// @brief Feed each of @p units, with its proof line, that the span of @p fed and the
    /// units fed before it does not hold.
    /// @pre As `feed_unit()`'s, for each unit fed.
    void feed_units(const std::vector<std::pair<Equation, ClauseId>>& units, RowSpace fed);

    /// @brief Delete an active clause: its proof line, its entry in the equivalent
    /// clause table, and the clause itself.
    void remove_clause(ClauseId id);

    /// @brief Make an active clause inactive and free its rows.
    /// @pre The clause is not in the equivalent clause table.
    void deactivate_clause(ClauseId id);

    /// @brief Hand clause @p retired_id's LBD, deletion state and CSIDS activity to @p kept_id,
    /// which takes its place; the caller deletes @p retired_id.
    void redirect_clause_state(ClauseId retired_id, ClauseId kept_id);

    /// @brief Leave clause @p kept_id the lower of its own and @p removed's measured LBDs, and
    /// the better deletion state of the two.
    void merge_deletion_state(ClauseId kept_id, const EvictedClauseState& removed);

#ifndef NDEBUG
    /// @brief Whether the table holds exactly the active clauses.
    bool debug_equivalent_clause_table_is_exact() const;

    /// @brief Whether @p narrowed is a narrowing of clause @p id, as `vivify_replace()`
    /// requires.
    /// @pre The trail is at the root.
    bool debug_is_narrowing(const Clause& narrowed, ClauseId id) const;

    /// @brief Whether no two active clauses have one span, checked only without an
    /// attempt budget and, on a large database, every so many calls.
    bool debug_no_two_clauses_share_a_span();
#endif

    EquationRepr repr_;

    const std::size_t original_num_vars_;
    const std::vector<std::size_t> used_vars_; ///< Ascending, in the original numbering.
    /// @brief The variables of the active numbering, the dropped ones included.
    std::size_t active_num_vars_;

    /// @brief Active to original variable index, composed across renumberings.
    std::vector<std::size_t> active_var_to_original_;

    /// @brief The variables dropped since the last renumbering, over
    /// `active_num_vars_ + 1` columns: zero in every active clause, and never on the trail.
    IndexSet dropped_col_idxs_;
    std::size_t num_dropped_vars_ = 0;

    std::vector<CompactionHistoryEntry> compaction_history_; ///< Oldest first.

    bool renumber_;
    EquivalentClauseTable equivalent_clauses_; ///< The active clauses.
    std::vector<ClauseId> evicted_clause_ids_; ///< Of the substitution under way.
    std::optional<DeletionConfig> deletion_;

#ifndef NDEBUG
    bool has_solved_ = false;
    std::size_t debug_quiescences_ = 0;
    std::size_t debug_span_checks_ = 0;
    std::size_t debug_reason_checks_ = 0;
#endif
    std::size_t conflicts_at_last_deletion_ = 0;
    std::size_t num_active_clauses_ = 0;

    Trail trail_;

    /// @brief The clause each decision level's decision came from, indexed by
    /// level less one, and empty where the fallback basis supplied it.
    std::vector<std::optional<ClauseId>> decision_clause_of_level_;

    ClauseDatabase clause_database_;
    std::unique_ptr<ClauseSelector> selector_;
    Cyclic* cyclic_selector_ = nullptr; ///< `selector_`, if it is `Cyclic`.
    std::unique_ptr<Csids> decisions_;
    std::unique_ptr<RestartPolicy> restarts_;

    /// @brief Add a clause to the database as a starting clause, and tell the selector.
    void add_starting_clause_to_database(Clause clause);

    /// @brief Add a learned clause to the database, and tell the selector; `trail_`
    /// already reflects the backjump.
    void add_asserting_clause_to_database(Clause clause, const AssertingClauseInfo& info);

    /// @brief Enter the schedule's mode.
    void switch_mode();

    /// @brief Present exactly under `SolverConfig::stabilize`.
    std::optional<ModeSchedule> modes_;
    std::optional<LbdRestarts> focused_restarts_;

    Propagator propagator_;

    ProofWriter* proof_writer_; ///< Not owned; null for no proof.

    /// @brief The next proof id, which every learned line takes in turn, root
    /// units included.
    ClauseId next_proof_clause_id_ = 0;

    /// @brief Each clause's proof id, indexed by clause id; empty for no proof.
    std::vector<ClauseId> proof_id_of_clause_;

    /// @brief The proof id of the first input clause with no rows, which the
    /// database never holds.
    std::optional<ClauseId> empty_starting_clause_proof_id_;

    /// @brief The root units since the last compaction over the original
    /// variables, each reduced by the ones before it.
    RowSpace proof_root_span_;
    std::vector<ClauseId> proof_root_unit_ids_; ///< Each unit's line, in trail order.
    /// @brief The units' lines that are no clause's line.
    std::vector<ClauseId> proof_unit_only_lines_;
    std::vector<ClauseId> proof_col_idx_to_unit_id_; ///< Pivot column to the unit's proof id.

    /// @brief A distinct stamp per decision level, reissued whenever a level is
    /// built again after a backjump.
    ///
    /// A clause's record of having been satisfied is thereby never read against
    /// a different level of the same depth. Index 0 is the root, whose span only
    /// ever grows.
    std::vector<std::size_t> level_stamp_{1};
    std::size_t next_level_stamp_ = 2;

    /// @brief The shallowest level whose span already satisfies @p row.
    ///
    /// Reducing @p row names the span rows it used, and the level filtration
    /// only ever grows, so the deepest level among them is the level the row
    /// first became satisfied at.
    /// @pre `row` reduces to `0 = 1` against the current span.
    std::size_t shallowest_satisfying_level(const Equation& row);

    /// @brief The deepest level among the trail rows `used_row_idxs` names, which
    /// is `0` when it names none.
    std::size_t level_of_used_rows(const IndexSet& used_row_idxs) const;

    /// @brief Whether the trail falsifies `clause` only at its top level, so that
    /// taking that level off leaves the clause at dimension one.
    /// @pre The trail falsifies `clause` and stands at the level that does.
    bool falsified_only_at_top_level(const Clause& clause) const;

    SatisfactionRecord satisfaction_record() const {
        return SatisfactionRecord(clause_database_.records(), level_stamp_, trail_.level());
    }

    /// @brief Whether the clause of @p record is known to be satisfied by a level at
    /// or below the current one, so that no attempt on it can find anything.
    bool clause_is_known_satisfied(const ClauseRecord& record) const {
        return satisfaction_record().satisfying_level(record).has_value();
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

    std::optional<PropagationConfig> attempt_budget_config_;

    /// @brief The attempt budget the miss rate steers, engaged for a nonzero one
    /// and rebuilt when a solve starts.
    std::optional<AttemptBudget> budget_;

    /// @brief Whether a vivification order is assuming rows.
    bool in_vivify_try_ = false;

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
    IndexSet satisfied_probe_row_idxs_; ///< The rows that reduction used.
    /// @brief Scratch for `implied_on_trail()`.
    Equation replay_implied_eqn_;
    Equation replay_reduced_eqn_;

    /// @brief What the propagation path counts, which a vivification try counts apart.
    struct PropagationCounters {
        std::size_t num_propagations = 0;
        std::size_t num_attempt_props = 0;
        std::size_t num_watch_updates = 0;
        std::size_t num_out_of_order_learns = 0;
    };
    PropagationCounters propagation_counters_;

    /// @brief The counters the solver keeps itself; `stats()` adds the rest.
    RunStats stats_;
    PhaseTimer phase_timer_;
    /// @brief Summed LBD of the asserting clauses, for their mean: a root conflict
    /// learns none, and a subsumption replacement is not one.
    std::size_t sum_learned_lbd_ = 0;
    std::size_t num_asserting_clauses_ = 0;
    /// @brief Summed trail level and rank *at* the conflicts, before their
    /// backjumps, for the means the progress line reports over an interval.
    std::size_t sum_conflict_level_ = 0;
    std::size_t sum_conflict_trail_length_ = 0;

    ProgressConfig progress_;

    /// @brief Whether conflict analysis minimizes the clauses it learns.
    bool minimize_;

    /// @brief The stream each minimization step's representative is drawn from.
    Rng representative_rng_;

    /// @brief Whether a conflict also bumps the clauses at the learned clause's
    /// pivots in the trail basis.
    bool bump_responsible_clauses_;

    /// @brief Whether a learned clause deletes the reason clause it subsumes.
    bool otf_subsume_;

    std::size_t lbd_window_;

    std::optional<BveConfig> bve_config_;
    /// @brief Until a BVE pass removes fewer than `success_rate` of the clauses it read.
    bool bve_is_due_;
    Rng bve_rng_;

    std::optional<ImplicationGraphConfig> implication_graph_config_;
    std::optional<ImplicationGraph> implication_graph_;

    /// @brief What the last run proved, filled only under a proof.
    ImplicationGraphProofRecord implication_graph_proof_;
    /// @brief Whether a run is due whatever the conflicts: before the first run.
    bool implication_graph_run_is_owed_ = true;
    /// @brief The conflicts at the last run or skip, and the width-2 clauses
    /// counted at the last run.
    std::size_t conflicts_at_last_implication_graph_point_ = 0;
    std::size_t width_2_clauses_at_last_implication_graph_run_ = 0;

    std::optional<VivifyConfig> vivify_;
    Rng vivify_rng_;
    std::size_t conflicts_at_last_vivify_pass_ = 0;
    /// @brief Decisions and propagations of the search when the last pass ran.
    std::size_t search_rows_at_last_vivify_pass_ = 0;
    bool vivify_has_run_ = false;
    std::size_t num_vivify_rows_assumed_ = 0; ///< Over all passes, for their budgets.

    /// @brief What a forced restart saved for its replay.
    struct Replay {
        /// @brief Each level's decision, rewritten with the clauses.
        SavedTrail decisions;
        std::vector<std::optional<ClauseId>> decision_clause_ids; ///< By level less one.
        /// @brief The reasons of each level's rows in trail order, all levels in one
        /// list, `reason_ends[k]` ending level `k + 1`'s.
        std::vector<ClauseId> reason_ids;
        std::vector<std::size_t> reason_ends;
        /// @brief Each clause the visit deleted with another in its place, to that one.
        std::unordered_map<ClauseId, ClauseId> kept_in_place_of;
        std::size_t next_level = 0; ///< The position of the next level in the saved list.
        /// @brief From the forced restart to a conflict or the step after the last level.
        bool under_way = false;
        bool has_replayed = false; ///< Whether the first decision's step has run.

        bool is_pending() const noexcept { return under_way; }

        /// @brief The clause standing for @p id after the visit's deletions.
        ClauseId kept_for(ClauseId id) const;
    };
    Replay replay_;

    std::optional<SubsumePassConfig> subsume_pass_;
    Rng subsume_pass_rng_;
    std::size_t conflicts_at_last_subsume_pass_ = 0;
    /// @brief Decisions and propagations of the search when the last pass ran.
    std::size_t search_rows_at_last_subsume_pass_ = 0;
    bool subsume_pass_has_run_ = false;

    /// @brief What the last progress line saw, so the next one can report the
    /// interval between them rather than the run so far.
    std::size_t conflicts_at_last_report_ = 0;
    std::size_t sum_level_at_last_report_ = 0;
    std::size_t sum_trail_length_at_last_report_ = 0;
    double time_at_last_report_s_ = 0.0;

    std::chrono::steady_clock::time_point solve_start_;
};

} // namespace xorcle
