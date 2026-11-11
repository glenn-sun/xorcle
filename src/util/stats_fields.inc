// -*- C++ -*-
//
/// @file stats_fields.inc
/// @brief The statistics table: one row per numeric field of a run record.
///
/// `src/util/stats.h` includes it to declare `RunStats`'s members, and
/// `src/util/stats_writer.cpp` includes it to write both output formats.
///
/// - `XORCLE_STAT(section, kind, name, csv_name, label)` — one field. `kind` is
///   `Count` (a `std::size_t`, printed as digits) or `Real` (a `double`,
///   printed to three decimals). `section` decides where the console block
///   prints it and whether it prints at all; see `StatsSection`.
///
/// Text fields, the verdict, the configuration and the derived ratios are not
/// rows: they are written by hand, because each needs its own formatting.

#ifndef XORCLE_STAT
#define XORCLE_STAT(section, kind, name, csv_name, label)
#endif

// --- The instance ----------------------------------------------------------

XORCLE_STAT(instance, Count, num_vars, "num_vars", "Variables")
XORCLE_STAT(instance, Count, unused_vars, "unused_vars", "Unused variables")
XORCLE_STAT(instance, Count, active_vars, "active_vars", "Active variables")
XORCLE_STAT(instance, Count, start_clauses, "start_clauses", "Start clauses")

// --- The search ------------------------------------------------------------

XORCLE_STAT(search, Count, num_conflicts, "conflicts", "Conflicts")
XORCLE_STAT(search, Count, decisions, "decisions", "Decisions")
XORCLE_STAT(search, Count, fallback_decisions, "fallback_decisions", "Fallback decisions")
XORCLE_STAT(search, Count, num_propagations, "propagations", "Propagations")
XORCLE_STAT(search, Count, num_attempt_props, "attempt_props", "Attempted propagations")
XORCLE_STAT(search, Count, num_watch_updates, "watch_updates", "Watch updates")
XORCLE_STAT(search, Count, num_compactions, "compactions", "Compactions")
XORCLE_STAT(search, Count, num_renumberings, "renumberings", "Compactions that renumbered")
XORCLE_STAT(search, Count, num_dropped_vars, "dropped_vars", "Variables dropped")
XORCLE_STAT(search, Count, num_substituted_clauses, "substituted_clauses", "Clauses rewritten by a compaction")
XORCLE_STAT(search, Count, num_tautologies_at_input, "tautologies_at_input", "Tautologies dropped at input")
XORCLE_STAT(search, Count, num_equivalent_clauses, "equivalent_clauses", "Equivalent clauses evicted")
XORCLE_STAT(search, Count, num_forced_restarts, "forced_restarts", "Forced restarts")
XORCLE_STAT(search, Count, forced_restart_rows, "forced_restart_rows", "Trail rows above the root forced restarts dropped")
XORCLE_STAT(search, Count, num_replayed_decisions, "replayed_decisions", "Decisions replayed")
XORCLE_STAT(search, Count, num_replay_implied_decisions, "replay_implied_decisions", "Saved decisions the trail implied at their replay")
XORCLE_STAT(search, Count, num_replay_refuted_decisions, "replay_refuted_decisions", "Saved decisions the trail refuted at their replay")
XORCLE_STAT(search, Count, num_replays_ended_by_conflict, "replays_ended_by_conflict", "Replays a conflict ended")

// --- Restarts, reported only when a schedule is in force --------------------

XORCLE_STAT(restart, Count, num_restarts, "restarts", "Restarts")
XORCLE_STAT(restart, Count, num_canceled_restarts, "canceled_restarts", "Canceled restarts")
XORCLE_STAT(restart, Real, fast_lbd, "fast_lbd", "Fast LBD average")
XORCLE_STAT(restart, Real, slow_lbd, "slow_lbd", "Slow LBD average")

// --- Stable and focused modes, reported only under --stabilize -------------

XORCLE_STAT(stabilize, Count, focused_restarts, "focused_restarts", "Restarts in focused mode")
XORCLE_STAT(stabilize, Count, focused_canceled_restarts, "focused_canceled_restarts", "Canceled restarts in focused mode")
XORCLE_STAT(stabilize, Real, focused_fast_lbd, "focused_fast_lbd", "Fast LBD average in focused mode")
XORCLE_STAT(stabilize, Real, focused_slow_lbd, "focused_slow_lbd", "Slow LBD average in focused mode")

// --- The clause database ---------------------------------------------------

XORCLE_STAT(clauses, Count, learned_clauses, "learned_clauses", "Learned clauses")
XORCLE_STAT(clauses, Count, clauses_in_database, "clauses_in_database", "Clauses in database")
XORCLE_STAT(clauses, Real, average_learned_lbd, "average_learned_lbd", "Average learned LBD")
XORCLE_STAT(clauses, Count, learned_width_1, "learned_width_1", "Learned width-1 clauses")
XORCLE_STAT(clauses, Count, learned_width_2, "learned_width_2", "Learned width-2 clauses")

// --- Phase saving, reported only when it is on -----------------------------

XORCLE_STAT(phase_saving, Count, phase_saving_open_sides, "phase_saving_open_sides", "Decisions whose side was left to --phase")
XORCLE_STAT(phase_saving, Count, phase_saving_decisions_both, "phase_saving_decisions_both", "Decisions in the rotation's step both")
XORCLE_STAT(phase_saving, Count, phase_saving_decisions_none, "phase_saving_decisions_none", "Decisions in the rotation's step none")
XORCLE_STAT(phase_saving, Count, phase_saving_decisions_target_only, "phase_saving_decisions_target_only", "Decisions in the rotation's step target-only")
XORCLE_STAT(phase_saving, Count, phase_saving_decisions_recent_only, "phase_saving_decisions_recent_only", "Decisions in the rotation's step recent-only")

// --- Clause minimization, reported only when it ran -------------------------

XORCLE_STAT(minimize, Count, num_minimized_clauses, "minimized_clauses", "Minimized clauses")
XORCLE_STAT(minimize, Count, minimize_width_before, "minimize_width_before", "Width before minimizing")
XORCLE_STAT(minimize, Count, minimize_width_after, "minimize_width_after", "Width after minimizing")

// --- Subsumption, reported only when it ran ----------------------------------

XORCLE_STAT(subsume, Count, num_subsumed_clauses, "subsumed_clauses", "Clauses subsumed, or strengthened by the subsumption pass")

// --- Vivification, reported only when it ran --------------------------------

XORCLE_STAT(vivify, Count, vivify_passes, "vivify_passes", "Vivification passes")
XORCLE_STAT(vivify, Count, vivify_narrowed, "vivify_narrowed", "Clauses narrowed")
XORCLE_STAT(vivify, Count, vivify_width_before, "vivify_width_before", "Their width before")
XORCLE_STAT(vivify, Count, vivify_width_after, "vivify_width_after", "Their width after")
XORCLE_STAT(vivify, Count, vivify_units, "vivify_units", "Narrowed to a unit")

// --- The subsumption pass, reported only when it ran -------------------------

XORCLE_STAT(subsume_pass, Count, subsume_pass_passes, "subsume_pass_passes", "Subsumption passes")

// --- Clause deletion, reported only when it ran -----------------------------

XORCLE_STAT(deletion, Count, num_deletion_passes, "deletion_passes", "Deletion passes")
XORCLE_STAT(deletion, Count, num_deleted_clauses, "deleted_clauses", "Deleted clauses")

// --- Deferred propagation, reported only under a budget ---------------------

XORCLE_STAT(propagate_budget, Count, num_out_of_order_learns, "out_of_order_learns", "Out-of-order learns")
XORCLE_STAT(propagate_budget, Count, num_undone_decisions, "undone_decisions", "Decisions undone by a missed propagation")

// --- BVE, reported only when it is on ----------------------------------------

XORCLE_STAT(bve, Count, bve_passes, "bve_passes", "BVE passes")
XORCLE_STAT(bve, Count, bve_eliminated, "bve_eliminated", "BVE eliminations")
XORCLE_STAT(bve, Count, bve_clauses_before, "bve_clauses_before", "Clauses before the first BVE pass")
XORCLE_STAT(bve, Count, bve_clauses_after, "bve_clauses_after", "Clauses after the last BVE pass")
XORCLE_STAT(bve, Count, bve_width_before, "bve_width_before", "Total width before the first BVE pass")
XORCLE_STAT(bve, Count, bve_width_after, "bve_width_after", "Total width after the last BVE pass")

// --- The implication graph, reported only when it ran -----------------------

XORCLE_STAT(implication_graph, Count, implication_graph_runs, "implication_graph_runs", "Implication graph runs")
XORCLE_STAT(implication_graph, Count, implication_graph_pairs_examined, "implication_graph_pairs_examined", "Of those, the pairs the rule examined")
XORCLE_STAT(implication_graph, Count, implication_graph_units_fed, "implication_graph_units_fed", "Units fed to the root trail")
XORCLE_STAT(implication_graph, Count, implication_graph_new_width_2_clauses, "implication_graph_new_width_2_clauses", "Width-2 clauses new since the last run, summed over the points a run was due")
XORCLE_STAT(implication_graph, Count, implication_graph_skipped_runs, "implication_graph_skipped_runs", "Due runs skipped for too few new width-2 clauses")
XORCLE_STAT(implication_graph, Count, implication_graph_points_new_0, "implication_graph_points_new_0", "Due points with no new width-2 clause")
XORCLE_STAT(implication_graph, Count, implication_graph_points_new_1_9, "implication_graph_points_new_1_9", "Due points with 1-9 new width-2 clauses")
XORCLE_STAT(implication_graph, Count, implication_graph_points_new_10_49, "implication_graph_points_new_10_49", "Due points with 10-49 new width-2 clauses")
XORCLE_STAT(implication_graph, Count, implication_graph_points_new_50_99, "implication_graph_points_new_50_99", "Due points with 50-99 new width-2 clauses")
XORCLE_STAT(implication_graph, Count, implication_graph_points_new_100, "implication_graph_points_new_100", "Due points with 100 or more new width-2 clauses")

// --- Timing ----------------------------------------------------------------

XORCLE_STAT(timing, Real, parse_time_s, "parse_time_s", "Parse time (s)")
XORCLE_STAT(timing, Real, solve_time_s, "solve_time_s", "Solve time (s)")

// --- Solve time by phase, which sums to the solve time ----------------------
//
// Each phase's own time: a technique's propagation and the units it feeds are the
// technique's; a renumbering is the substitution's wherever it runs; a decision
// includes phase saving's save of the trail at each conflict, and opening its level
// is propagation; a forced restart's save is the replay's.

XORCLE_STAT(solve_time, Real, decision_time_s, "decision_time_s", "Decisions")
XORCLE_STAT(solve_time, Real, propagation_time_s, "propagation_time_s", "Propagation")
XORCLE_STAT(solve_time, Real, analysis_time_s, "analysis_time_s", "Conflict analysis")
XORCLE_STAT(solve_time, Real, deletion_time_s, "deletion_time_s", "Deletion")
XORCLE_STAT(solve_time, Real, substitution_time_s, "substitution_time_s", "Substitution and renumbering")
XORCLE_STAT(solve_time, Real, bve_time_s, "bve_time_s", "BVE")
XORCLE_STAT(solve_time, Real, subsume_pass_time_s, "subsume_pass_time_s", "Subsumption pass")
XORCLE_STAT(solve_time, Real, vivify_time_s, "vivify_time_s", "Vivification")
XORCLE_STAT(solve_time, Real, implication_graph_time_s, "implication_graph_time_s", "Implication graph")
XORCLE_STAT(solve_time, Real, replay_time_s, "replay_time_s", "Replay")
XORCLE_STAT(solve_time, Real, other_time_s, "other_time_s", "Other")

#undef XORCLE_STAT
