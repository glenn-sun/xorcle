#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/row_echelon_matrix.h"
#include "core/clause_id.h"
#include "core/propagation.h"
#include "core/trail.h"
#include "heuristics/bve/bve.h"
#include "heuristics/compaction/compaction.h"
#include "heuristics/decisions/clause_activity.h"
#include "heuristics/decisions/decision_procedure.h"
#include "heuristics/deletion/deletion.h"
#include "heuristics/equivalent_clauses/equivalent_clause_table.h"
#include "heuristics/implication_graph/implication_graph.h"
#include "heuristics/propagation/attempt_budget.h"
#include "heuristics/restarts/restart_policy.h"
#include "heuristics/restarts/stabilize.h"
#include "heuristics/selection/clause_selector.h"
#include "heuristics/selection/cyclic.h"
#include "io/proof_raw_file.h"
#include "util/index_set.h"
#include "util/progress_reporter.h"
#include "util/random.h"
#include "util/stats.h"

namespace xorcle {

/// @brief The trail an LRUP(⊕) checker builds while it reads one proof line.
class HintTrail;

/// @brief When the solver is allowed to compact the variable numbering.
struct CompactionConfig {
    bool enabled = true;

    bool operator==(const CompactionConfig&) const = default;
};

/// @brief How far a propagation run may go before deferring to a decision.
struct PropagationConfig {
    /// @brief The fraction of propagated equations the attempt budget steers
    /// toward learning out of order; `0` runs every propagation to quiescence.
    double miss_rate = 0.0;

    /// @brief Learned equations over which the attempt budget falls by a factor
    /// of `e` when none of them is missed.
    std::size_t window = 2000;

    bool operator==(const PropagationConfig&) const = default;
};

/// @brief When and how vivification narrows clauses.
///
/// A pass goes through the clauses of width 3 and up by width, narrowest first,
/// tries a clause of width `w` in a round with probability `2^(3 - w)`, and leaves
/// out a clause a pass went through unless it is new or narrowed since.
struct VivifyConfig {
    bool enabled = false;

    /// @brief Times a pass goes through the clauses of one width before the next width.
    std::size_t rounds = 2;

    std::size_t cadence = 1000; ///< Conflicts after a pass before the next may run.

    /// @brief Whether a pass due above the root goes to the root for it and then
    /// replays the decisions.
    bool force_root_visit = false;

    /// @brief The rows a pass may assume and imply, per row the search decided or
    /// propagated since the last pass.
    double effort = 0.01;

    bool operator==(const VivifyConfig&) const = default;
};

/// @brief A clause an order narrowed to, with the proof lines that derive it.
struct VivifyNarrowing {
    Clause clause;
    /// @brief In replay order; empty without a proof.
    std::vector<ClauseId> proof_hints;
};

/// @brief What the vivification passes of one solve did.
struct VivifyCounters {
    std::size_t num_passes = 0;
    std::size_t num_candidates = 0; ///< Clauses listed, summed over the passes.
    /// @brief Clauses left unlisted as already gone through, summed over the passes.
    std::size_t num_unchanged = 0;
    std::size_t num_orders = 0;
    std::size_t num_assumed = 0;       ///< Rows assumed, each opening a level.
    std::size_t num_implied = 0;       ///< Equations the assumptions implied.
    std::size_t num_attempt_props = 0; ///< Clauses handed to the propagator for them.

    std::size_t num_narrowed = 0;
    std::size_t narrowed_width_before = 0; ///< Summed over the narrowed clauses.
    std::size_t narrowed_width_after = 0;
    std::size_t num_units = 0;
    std::size_t num_narrowed_to_width_2 = 0;

    /// @brief The narrowed clauses by what ended the order.
    std::size_t num_narrowed_by_refutation = 0;
    std::size_t num_narrowed_by_conflict = 0;

    /// @brief Conflict analyses that resolved with a clause a pass put in place.
    std::size_t num_replacement_uses = 0;

    std::size_t num_root_satisfied = 0; ///< Orders that found the root satisfying the clause.


    /// @brief Root visits forced for a pass, the trail rows above the root they
    /// dropped, and the decisions replayed and left out as decided.
    std::size_t num_forced_visits = 0;
    std::size_t forced_visit_rows = 0;
    std::size_t num_replayed = 0;
    std::size_t num_replay_skipped = 0;


    std::size_t num_budget_stops = 0; ///< Passes their budget stopped before their last order.
    double time_s = 0.0;
};

/// @brief When the subsumption pass runs.
///
/// A pass at a root visit checks pairs of active clauses of which one was added or
/// changed since the last pass: a subsumed wider clause D is deleted, and a
/// strengthened D loses a row. It visits every clause as the narrower clause,
/// narrowest first, then each unchanged clause as the wider clause, until its
/// budget is spent. A pass due above the root goes to the root for it and replays
/// the decisions.
struct SubsumePassConfig {
    bool enabled = false;

    std::size_t cadence = 1000; ///< Conflicts after a pass before the next may run.

    /// @brief The rows a pass may read in its checks per row the search decided or
    /// implied since the last pass.
    double effort = 10.0;

    bool operator==(const SubsumePassConfig&) const = default;
};

/// @brief Hits of the subsumption pass by the kinds of the narrower clause C and
/// the wider clause D, `starting` or `learned` as each was before the hit.
struct SubsumePassHitsByKind {
    std::size_t starting_by_starting = 0;
    std::size_t learned_by_starting = 0; ///< D learned, C starting.
    std::size_t starting_by_learned = 0; ///< D starting, C learned.
    std::size_t learned_by_learned = 0;
};

/// @brief What the subsumption passes of one solve did.
struct SubsumePassCounters {
    std::size_t num_passes = 0;
    std::size_t num_forced_visits = 0;

    /// @brief Visits of a clause as the wider clause D and as the narrower clause C.
    std::size_t num_visits_as_wider = 0;
    std::size_t num_visits_as_narrower = 0;

    std::size_t num_entries_scanned = 0;     ///< Index entries read, both directions.
    std::size_t num_containment_tests = 0;   ///< Of the leading columns, before any row.
    std::size_t num_row_checks = 0;          ///< Rows of C reduced against D.
    std::size_t num_row_additions = 0;       ///< Rows of D added in those reductions.
    std::size_t num_rows_read = 0;           ///< Per row of C reduced, the rows of D.
    std::size_t num_budget_stops = 0;        ///< Passes their budget stopped.

    /// @brief By the direction that found the hit: the visited wider or narrower.
    SubsumePassHitsByKind subsumed_as_wider;
    SubsumePassHitsByKind subsumed_as_narrower;
    SubsumePassHitsByKind strengthened_as_wider;
    SubsumePassHitsByKind strengthened_as_narrower;

    /// @brief The rows of the subsumed clauses, and one per strengthening.
    std::size_t num_rows_removed = 0;
    std::size_t num_units = 0;
    std::size_t num_promotions = 0; ///< Learned clauses that subsumed a starting one.

    /// @brief Hits by the width of D: up to 2, 3, 4 to 6, 7 and up.
    std::size_t hits_by_width[4] = {};
    /// @brief Hits on a learned D by its LBD: up to 2, up to 6, up to 15, above.
    std::size_t hits_by_lbd[4] = {};

    double time_s = 0.0;
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
    std::size_t num_compactions = 0;
    std::size_t num_renumberings = 0; ///< Compactions that also renumbered.
    std::size_t num_dropped_vars = 0;
    std::size_t num_substituted_clauses = 0; ///< Clauses a compaction rewrote or deactivated.
    std::size_t num_tautologies_at_input = 0;

    /// @brief Equivalent clauses evicted, by where the kept clause was found.
    std::size_t num_equivalent_clauses_at_input = 0;
    std::size_t num_equivalent_clauses_at_subsume_pass = 0;
    std::size_t num_equivalent_clauses_at_subsumption = 0;
    std::size_t num_equivalent_clauses_at_substitution = 0;
    std::size_t num_equivalent_clauses_at_vivification = 0;
    /// @brief Of those, the pairs of a starting and a learned clause, and of two learned clauses.
    std::size_t num_equivalent_starting_learned_pairs = 0;
    std::size_t num_equivalent_learned_pairs = 0;

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

    double decision_time_s = 0.0; ///< Spent choosing decisions, in the decision procedure.
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

    /// @brief Learned clauses of width 1 and of width 2 as they were added.
    std::size_t num_learned_width_1 = 0;
    std::size_t num_learned_width_2 = 0;

    /// @brief Clauses that reached width at most 2 by another route: a
    /// subsumption replacement, and a compaction narrowing a wider clause.
    std::size_t num_subsume_narrowed_to_width_2 = 0;
    std::size_t num_compaction_narrowed_to_width_2 = 0;
};

/// @brief What the implication graph runs of one solve did.
struct ImplicationGraphCounters {
    std::size_t num_runs = 0; ///< Runs that had a graph to read.
    std::size_t num_edges_visited = 0;

    /// @brief Vertex pairs with an out-arc on either side, the ones of those the
    /// pair cap left room for, and the sides of those whose span holds `0 = 1`.
    std::size_t num_pairs_eligible = 0;
    std::size_t num_pairs_examined = 0;
    std::size_t num_refuted_sides = 0;

    /// @brief Runs the pair cap stopped short, and sides the descendant cap
    /// truncated.
    std::size_t num_runs_at_the_pair_cap = 0;
    std::size_t num_truncated_sides = 0;

    std::size_t num_units_fed = 0;

    /// @brief What made each run due; the one run left over is the first of the
    /// solve, which is due because the graph has never been read.
    std::size_t num_runs_due_to_clauses = 0;
    std::size_t num_runs_due_to_conflicts = 0;
    std::size_t num_runs_due_to_a_feed = 0;

    /// @brief Follow-up runs withheld because too few clauses reached width at
    /// most 2 since the run that fed units.
    std::size_t num_follow_ups_suppressed = 0;

    /// @brief Runs past the first conflict, which the wait for the root trail
    /// makes rarer than the cadence alone would.
    std::size_t num_runs_after_a_conflict = 0;

    /// @brief Conflicts between a run falling due and the trail being back at
    /// the root for it, summed over the runs and at their worst.
    std::size_t num_conflicts_waited = 0;
    std::size_t max_conflicts_waited = 0;

    double span_intersection_time_s = 0.0; ///< Of the graph's own seconds.
    double time_s = 0.0;
};

/// @brief The CDCL(⊕) solver.
///
/// The loop is propagate, then either analyse a conflict, report SAT, compact,
/// run the implication graph, or decide. The `ClauseSelector` is also the authoritative clause store.
class Solver {
public:
    /// @brief Construct a solver over the variables `used_vars` of `num_vars`, with
    /// an empty clause database.
    ///
    /// Clauses, `selector` and `decisions` are over `used_vars.size()` variables, bit
    /// `i` being variable `used_vars[i]`. Models and proofs are over `num_vars`, the
    /// unused variables false in a model.
    ///
    /// `proof_writer` is null for no proof and is not owned, so it must outlive
    /// the solver, and the caller finalizes it.
    ///
    /// @pre `used_vars` is strictly ascending and below `num_vars`.
    /// @pre `selector`, `decisions` and `restarts` are all non-null; use
    /// `NoRestarts` to disable restarts.
    /// @pre A nonzero `propagation.miss_rate` comes with a `Cyclic` selector.
    /// @pre If `proof_writer` is non-null then `proof_writer->num_vars() == num_vars`.
    /// @pre If deletion is enabled with `DeletionKey::activity` then `decisions`
    /// is a `ClauseActivitySource`.
    /// @pre If `bve.enabled` then compaction is enabled.
    /// @pre If `stabilize.enabled` then `restarts` is a `NoRestarts`.
    Solver(EquationRepr repr, std::size_t num_vars, std::vector<std::size_t> used_vars,
           std::unique_ptr<ClauseSelector> selector, std::unique_ptr<DecisionProcedure> decisions,
           std::unique_ptr<RestartPolicy> restarts, RawProofWriter* proof_writer = nullptr,
           const CompactionConfig& compaction = {}, const ProgressConfig& progress = {},
           bool minimize = true, const DeletionConfig& deletion = {},
           bool bump_responsible_clauses = false, bool subsume = true,
           const PropagationConfig& propagation = {},
           const ImplicationGraphConfig& implication_graph = {},
           const VivifyConfig& vivify = {}, const BveConfig& bve = {},
           const SubsumePassConfig& subsume_pass = {},
           const StabilizeConfig& stabilize = {});

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
    /// selector stores it, so every input clause enters at activity zero.
    void add_starting_clause(Clause clause);

    Solution solve();

    /// @brief Original variable count, the unused variables included.
    std::size_t num_vars() const noexcept { return original_num_vars_; }

    /// @brief Clause ids ever handed out, which only grows: deletion deactivates
    /// rather than reclaiming them.
    std::size_t num_clauses() const noexcept { return selector_->num_clauses(); }

    /// @brief Clauses still in the database: neither deleted nor dropped by a
    /// compaction.
    std::size_t num_active_clauses() const noexcept { return num_active_clauses_; }

    /// @brief Used variables left after compaction.
    std::size_t active_num_vars() const noexcept { return active_num_vars_ - num_dropped_vars_; }

    std::size_t level() const noexcept { return trail_.level(); }

    const ClauseSelector& selector() const noexcept { return *selector_; }

    const SolverCounters& counters() const noexcept { return counters_; }

    const ImplicationGraphCounters& implication_graph_counters() const noexcept {
        return implication_graph_counters_;
    }

    /// @brief Width-2 clauses and vertices as the graph last held them.
    const ImplicationGraph& implication_graph() const noexcept { return implication_graph_; }

    const VivifyCounters& vivify_counters() const noexcept { return vivify_counters_; }

    const SubsumePassCounters& subsume_pass_counters() const noexcept {
        return subsume_pass_counters_;
    }

    const DecisionProcedure& decision_procedure() const noexcept { return *decisions_; }

    /// @brief Add the counters of the solver and of its heuristics to `stats`.
    void add_to_stats(RunStats& stats) const;

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

    /// @brief Cite the lines of the root units @p rows needs before a hint can
    /// name the clause holding them, stopping at one that falsifies a clause.
    Citations cite_root_units(std::span<const Equation> rows, HintTrail& trail);

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

    /// @brief Decide the next decision `maybe_force_root_visit()` saved that
    /// the trail leaves open, or as `make_decision_step()` once none is left.
    DecisionStep replay_decision_step();

    /// @brief With a vivification pass whose visit is forced or a subsumption pass
    /// due above the root, save the decisions to replay and go to the root, where
    /// the passes run once propagation is quiescent.
    /// @return Whether it went to the root.
    bool maybe_force_root_visit();

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

    /// @brief Compact if the trigger conditions hold; true if the root trail was
    /// emptied, in which case the caller must restart the loop rather than decide.
    bool maybe_compact_before_decision();

    /// @brief Rewrite the clauses by a prepared plan and drop the variables the
    /// root trail fixes.
    ///
    /// @warning This discards the root trail, so root-level units become
    /// unrecoverable from the clause database.
    void apply_substitution(SubstitutionPlan plan);

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
#endif

    /// @brief Delete learned clauses if the conflict interval has elapsed.
    void maybe_delete_clauses();

    /// @brief Read the implication graph if a run is due and the trail is at
    /// the root, and add the units it finds.
    /// @return Whether the root trail gained clauses, in which case the caller
    /// must propagate again rather than decide.
    bool maybe_run_implication_graph();

    /// @brief Run a BVE pass over the clause database and drop the variables it
    /// takes out of every clause; a pass eliminating nothing changes nothing.
    ///
    /// A unit among the resolvents leaves the renumbering to the substitution it
    /// brings; otherwise the pass renumbers if that frees a word.
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

    /// @brief Clauses that have reached width at most 2 over the whole solve, by
    /// any of the routes there.
    std::size_t clauses_reaching_width_at_most_2() const noexcept {
        return counters_.num_learned_width_1 + counters_.num_learned_width_2 +
               counters_.num_subsume_narrowed_to_width_2 +
               counters_.num_compaction_narrowed_to_width_2 + vivify_counters_.num_units +
               vivify_counters_.num_narrowed_to_width_2 +
               bve_totals_.num_resolvents_of_width_at_most_2;
    }

    /// @brief Run a vivification pass if the trail is at the root and one is owed.
    /// @return Whether the pass ran an order, in which case the caller must
    /// propagate again rather than decide.
    bool maybe_vivify();

    /// @brief The active clauses at least `vivify_min_width` wide that are the reason
    /// of no trail row and that no pass went through since they were added or narrowed, by width.
    std::vector<std::vector<ClauseId>> vivify_candidates_by_width();

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
    /// @pre The trail up to @p last_level, with @p narrowed's linear negation,
    /// propagates to a conflict.
    std::vector<ClauseId> vivify_proof_hints(const Clause& narrowed, std::size_t last_level) const;


    /// @brief Open a decision level with @p reduced_eqn, as a decision does.
    /// @pre `reduced_eqn` is reduced by the trail span and leads.
    void vivify_assume(const Equation& reduced_eqn);

    /// @brief Whether @p narrowed is a narrowing of clause @p id: its rows lie in
    /// the span of that clause's rows and the root rows, so that it alone implies
    /// the clause.
    /// @pre The trail is at the root.
    bool vivify_is_narrowing(const Clause& narrowed, ClauseId id) const;

    /// @brief Overwrite clause @p id with @p narrowing, hand the clause's deletion
    /// state to a kept clause with its span instead, or keep it, with its proof
    /// line, in @p units if it is one row.
    /// @return Whether the clause goes on under its id.
    /// @pre `vivify_is_narrowing(narrowing.clause, id)`.
    bool vivify_replace(ClauseId id, VivifyNarrowing narrowing,
                        std::vector<std::pair<Equation, ClauseId>>& units);

    /// @brief Overwrite clause @p id with @p narrowed at the root, or hand its
    /// deletion state to a kept clause with that span and delete it.
    /// @param site The counter of an eviction into a kept clause.
    /// @return Whether the clause goes on under its id.
    /// @pre @p narrowed implies the clause, is in reduced row-echelon form and has
    /// at least two rows; @p proof_hints derive it under a proof.
    bool put_narrowing_in_place(ClauseId id, Clause narrowed,
                                const std::vector<ClauseId>& proof_hints,
                                std::size_t SolverCounters::* site);

    /// @brief Run a subsumption pass if the trail is at the root and one is due.
    /// @return Whether the pass changed the clauses, in which case the caller must
    /// propagate again rather than decide.
    bool maybe_subsume_pass();

    /// @brief Run one subsumption pass; whether it changed the clauses.
    /// @pre The trail is at the root and quiescent.
    bool run_subsume_pass();

    /// @brief Apply a prepared plan and clear the used flags.
    void apply_deletion(const DeletionPlan& plan);

    /// @brief The deletion state of a clause, as its equivalent clause takes it over.
    struct EvictedClauseState {
        bool is_learned;
        double lbd; ///< Read only of a learned clause.
        std::size_t unused_passes;
        bool is_out_of_unused_passes;
        bool is_used_since_deletion;
    };

    EvictedClauseState evicted_clause_state(ClauseId id) const;

    /// @brief Count an eviction at @p site and leave the kept clause the better
    /// deletion state of the two.
    void merge_into_equivalent_clause(ClauseId kept_id, const EvictedClauseState& evicted,
                                      std::size_t SolverCounters::* site);

    /// @brief Leave clause @p kept_id the better deletion state of its own and @p removed.
    void merge_deletion_state(ClauseId kept_id, const EvictedClauseState& removed);

#ifndef NDEBUG
    /// @brief Whether the table holds exactly the active clauses.
    bool debug_equivalent_clause_table_is_exact() const;

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

    CompactionConfig compaction_;
    EquivalentClauseTable equivalent_clauses_; ///< The active clauses.
    std::vector<ClauseId> evicted_clause_ids_; ///< Of the substitution under way.
    DeletionConfig deletion_;
    /// @brief `decisions_` as the score `DeletionKey::activity` reads; not owned.
    const ClauseActivitySource* clause_activity_ = nullptr;

#ifndef NDEBUG
    bool has_solved_ = false;
    std::size_t debug_quiescences_ = 0;
    std::size_t debug_span_checks_ = 0;
#endif
    std::size_t conflicts_at_last_deletion_ = 0;
    std::size_t num_active_clauses_ = 0;

    Trail trail_;

    /// @brief The clause each decision level's decision came from, indexed by
    /// level less one, and empty where the fallback basis supplied it.
    std::vector<std::optional<ClauseId>> decision_clause_of_level_;

    std::unique_ptr<ClauseSelector> selector_;
    Cyclic* cyclic_selector_ = nullptr; ///< `selector_`, if it is `Cyclic`.
    std::unique_ptr<DecisionProcedure> decisions_;
    std::unique_ptr<RestartPolicy> restarts_;

    /// @brief What the search spent in a mode, or the run's totals at a mode's start.
    struct ModeTotals {
        std::size_t conflicts = 0;
        std::size_t decisions = 0;
        std::size_t propagations = 0;
        double time_s = 0.0;
    };

    /// @brief The run's totals so far.
    ModeTotals mode_totals_now() const;

    /// @brief Enter the schedule's mode, crediting what was spent since the last switch.
    void switch_mode();

    /// @brief Present exactly under `StabilizeConfig::enabled`.
    std::optional<ModeSchedule> modes_;
    std::unique_ptr<RestartPolicy> focused_restarts_;
    std::size_t interval_conflicts_ = 0;
    std::array<ModeTotals, 2> mode_totals_{}; ///< By `SearchMode`, up to the last switch.
    ModeTotals totals_at_mode_start_;
    std::size_t num_mode_switches_ = 0;
    std::size_t num_focused_restarts_ = 0;

    Propagator propagator_;

    RawProofWriter* proof_writer_; ///< Not owned; null for no proof.

    /// @brief The next proof id, which every learned line takes in turn, root
    /// units included.
    ClauseId next_proof_clause_id_ = 0;

    /// @brief Each clause's proof id, indexed by clause id; empty for no proof.
    std::vector<ClauseId> proof_id_of_clause_;

    /// @brief The proof id of the first input clause with no rows, which the
    /// database never holds.
    std::optional<ClauseId> empty_starting_clause_proof_id_;

    /// @brief The next proof id when the last compaction ran: the clauses below
    /// it may since have been rewritten.
    ClauseId first_proof_id_of_epoch_ = 0;

    /// @brief The root units since the last compaction over the original
    /// variables, each reduced by the ones before it.
    RowSpace proof_root_span_;
    std::vector<RawUnit> proof_root_units_;   ///< In trail order.
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
        return SatisfactionRecord(level_stamp_, trail_.level());
    }

    /// @brief Whether `clause` is known to be satisfied by a level at or below
    /// the current one, so that no attempt on it can find anything.
    bool clause_is_known_satisfied(const Clause& clause) const {
        return satisfaction_record().satisfying_level(clause).has_value();
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
    IndexSet satisfied_probe_row_idxs_; ///< The rows that reduction used.

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

    BveConfig bve_config_;
    /// @brief Until the BVE pass has run.
    bool bve_is_due_;
    Rng bve_rng_;
    BveTotals bve_totals_;
    std::optional<std::size_t> vars_at_first_decision_;

    ImplicationGraphConfig implication_graph_config_;
    ImplicationGraph implication_graph_;

    /// @brief What the last run proved, filled only under a proof.
    ImplicationGraphProofRecord implication_graph_proof_;
    ImplicationGraphCounters implication_graph_counters_;
    /// @brief Whether a run is owed: before the first one, once the conflict
    /// interval has elapsed since the last, and after one that fed units if
    /// the clause cadence has been met since it.
    bool implication_graph_run_is_due_ = true;
    std::size_t conflicts_at_last_implication_graph_run_ = 0;
    std::size_t clauses_at_last_implication_graph_run_ = 0;
    std::size_t conflicts_at_run_became_due_ = 0;
    /// @brief Whether a run fed units and the follow-up clause test is still owed.
    bool follow_up_is_pending_ = false;

    /// @brief Which of the `ImplicationGraphCounters` to credit the run that is due to.
    enum class RunTrigger : std::uint8_t { first_run, clauses, conflicts, feed };
    RunTrigger run_is_due_from_ = RunTrigger::first_run;

    VivifyConfig vivify_;
    VivifyCounters vivify_counters_;
    Rng vivify_rng_;
    std::size_t conflicts_at_last_vivify_pass_ = 0;
    /// @brief Decisions and propagations of the search when the last pass ran.
    std::size_t search_rows_at_last_vivify_pass_ = 0;
    /// @brief Whether a pass has run since the trail last left the root.
    bool vivify_passed_at_this_root_visit_ = false;
    /// @brief By clause id, short of the newest ids: whether vivification narrowed it.
    std::vector<std::uint8_t> is_vivify_replacement_;

    /// @brief The decisions a forced root visit left to replay, each with its
    /// decision clause, and the next to replay.
    std::vector<std::pair<Equation, std::optional<ClauseId>>> root_visit_replay_;
    std::size_t root_visit_replay_next_ = 0;

    SubsumePassConfig subsume_pass_;
    SubsumePassCounters subsume_pass_counters_;
    Rng subsume_pass_rng_;
    std::size_t conflicts_at_last_subsume_pass_ = 0;
    /// @brief Decisions and propagations of the search when the last pass ran.
    std::size_t search_rows_at_last_subsume_pass_ = 0;
    /// @brief Whether a pass has run since the trail last left the root.
    bool subsume_passed_at_this_root_visit_ = false;

    /// @brief What the last progress line saw, so the next one can report the
    /// interval between them rather than the run so far.
    std::size_t conflicts_at_last_report_ = 0;
    std::size_t sum_level_at_last_report_ = 0;
    std::size_t sum_trail_length_at_last_report_ = 0;
    double time_at_last_report_s_ = 0.0;

    std::chrono::steady_clock::time_point solve_start_;
};

} // namespace xorcle
