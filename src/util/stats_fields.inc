// -*- C++ -*-
//
/// @file stats_fields.inc
/// @brief The statistics table: one row per numeric field of a run record.
///
/// `src/util/stats.h` includes it to declare `RunStats`'s members, and
/// `src/util/stats_writer.cpp` includes it to write both output formats.
///
/// - `XORCLE_STAT(section, kind, name, csv_name, label)` — one field. `kind` is
///   `Count` (a `std::size_t`, printed as digits), `SignedCount` (a
///   `std::int64_t`, printed as digits), `Real` (a `double`, printed to three
///   decimals) or `Average` (a `std::optional<double>`, empty when its
///   denominator is zero: the console then leaves the line out and the CSV
///   writes 0). `section` decides where the console block prints it and whether
///   it prints at all; see `StatsSection`.
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
XORCLE_STAT(search, Count, num_propagations, "propagations", "Propagations")
XORCLE_STAT(search, Count, num_attempt_props, "attempt_props", "Attempted propagations")
XORCLE_STAT(search, Count, num_watch_updates, "watch_updates", "Watch updates")
XORCLE_STAT(search, Count, num_dropped_vars, "dropped_vars", "Variables dropped")
XORCLE_STAT(search, Count, num_tautologies_at_input, "tautologies_at_input", "Tautologies dropped at input")
XORCLE_STAT(search, Count, num_equivalent_clauses, "equivalent_clauses", "Equivalent clauses removed")
XORCLE_STAT(search, Count, num_forced_restarts, "replayed_restarts", "Replayed restarts for inprocessing")

// --- Restarts, reported only when a schedule is in force --------------------

XORCLE_STAT(restart, Count, num_restarts, "restarts", "Restarts")
XORCLE_STAT(restart, Count, num_canceled_restarts, "cancelled_restarts", "Restarts cancelled by long trails")

// --- Stable and focused modes, reported only under --stabilize -------------

XORCLE_STAT(stabilize, Count, focused_restarts, "focused_restarts", "Restarts in focused mode")
XORCLE_STAT(stabilize, Count, focused_canceled_restarts, "focused_cancelled_restarts", "Restarts cancelled by long trails in focused mode")
XORCLE_STAT(stabilize, Real, focused_fast_lbd, "focused_fast_lbd", "Fast LBD average in focused mode")
XORCLE_STAT(stabilize, Real, focused_slow_lbd, "focused_slow_lbd", "Slow LBD average in focused mode")

// --- The clause database ---------------------------------------------------

XORCLE_STAT(clauses, Count, clauses_in_database, "final_clauses", "Clauses in final database")
XORCLE_STAT(clauses, Real, average_learned_lbd, "average_learned_lbd", "Average learned LBD")
XORCLE_STAT(clauses, Count, learned_width_1, "learned_units", "Learned units (all sources)")
XORCLE_STAT(clauses, Count, learned_width_2, "learned_width_2", "Learned width-2 clauses (all sources)")

// --- Phase saving, reported only when it is on -----------------------------

XORCLE_STAT(phase_saving, Count, phase_saving_open_sides, "phase_default_decisions", "Decisions left to --phase-default")

// --- Clause minimization, reported only when it ran -------------------------

XORCLE_STAT(minimize, Count, num_minimized_clauses, "minimized_clauses", "Minimized clauses")
XORCLE_STAT(minimize, Average, minimize_average_width_removed, "minimize_average_width_removed", "Average width removed per minimized clause")

// --- On-the-fly subsumption, reported only when it is on --------------------

XORCLE_STAT(otf_subsume, Count, num_otf_subsumed_clauses, "otf_subsumed_clauses", "Clauses subsumed on-the-fly")

// --- Vivification, reported only when it ran --------------------------------

XORCLE_STAT(vivify, Count, vivify_narrowed, "vivified_clauses", "Clauses vivified")
XORCLE_STAT(vivify, Average, vivify_average_width_removed, "vivify_average_width_removed", "Average width removed per vivified clause")

// --- The subsumption pass, reported only when it ran -------------------------

XORCLE_STAT(subsume_pass, Count, num_inproc_subsumed_clauses, "inproc_subsumed_clauses", "Clauses subsumed as inprocessing")
XORCLE_STAT(subsume_pass, Count, num_inproc_strengthened_clauses, "inproc_strengthened_clauses", "Clauses strengthened as inprocessing")

// --- Clause deletion, reported only when it ran -----------------------------

XORCLE_STAT(deletion, Count, num_deleted_clauses, "deleted_clauses", "Deleted clauses")

// --- Deferred propagation, reported only under a budget ---------------------

XORCLE_STAT(propagate_budget, Count, num_out_of_order_learns, "out_of_order_learns", "Out-of-order learns")
XORCLE_STAT(propagate_budget, Count, num_undone_decisions, "undone_decisions", "Decisions undone by a missed propagation")

// --- BVE, reported only when it is on ----------------------------------------

XORCLE_STAT(bve, Count, bve_passes, "bve_passes", "BVE passes")
XORCLE_STAT(bve, Count, bve_eliminated, "bve_successes", "BVE successful projections")
XORCLE_STAT(bve, SignedCount, bve_clause_count_difference, "bve_clause_count_difference", "Difference in clause count after BVE")
XORCLE_STAT(bve, SignedCount, bve_total_width_difference, "bve_total_width_difference", "Difference in total width after BVE")
XORCLE_STAT(bve, Count, bve_gate_successes, "bve_gate_successes", "BVE successful projections via gate detection")
XORCLE_STAT(bve, Count, bve_root_equations, "bve_gate_units", "Units from one-sided gates")
XORCLE_STAT(bve, Real, bve_gate_time_s, "bve_gate_time_s", "Time of the gate searches (s)")

// --- The implication graph, reported only when it ran -----------------------

XORCLE_STAT(implication_graph, Count, implication_graph_runs, "implication_graph_runs", "Implication graph runs")
XORCLE_STAT(implication_graph, Average, implication_graph_pairs_per_unit, "implication_graph_pairs_per_unit", "Attempted vertex pairs per learned unit")
XORCLE_STAT(implication_graph, Count, implication_graph_units_fed, "implication_graph_units_learned", "Units learned from implication graph")
XORCLE_STAT(implication_graph, Average, implication_graph_average_new_width_2_clauses, "implication_graph_average_new_width_2_clauses", "Average new width-2 clauses for implication graph runs")
XORCLE_STAT(implication_graph, Count, implication_graph_skipped_runs, "implication_graph_skipped_runs", "Skipped implication graph runs")

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
XORCLE_STAT(solve_time, Real, substitution_time_s, "substitution_time_s", "Substitution and compaction")
XORCLE_STAT(solve_time, Real, bve_time_s, "bve_time_s", "BVE")
XORCLE_STAT(solve_time, Real, subsume_pass_time_s, "inproc_subsume_strengthen_time_s", "Subsumption and strengthening")
XORCLE_STAT(solve_time, Real, vivify_time_s, "vivify_time_s", "Vivification")
XORCLE_STAT(solve_time, Real, implication_graph_time_s, "implication_graph_time_s", "Implication graph")
XORCLE_STAT(solve_time, Real, replay_time_s, "replay_time_s", "Replay")
XORCLE_STAT(solve_time, Real, other_time_s, "other_time_s", "Other")

#undef XORCLE_STAT
