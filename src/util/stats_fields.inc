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
XORCLE_STAT(search, Count, num_equivalent_clauses, "equivalent_clauses", "Equivalent clauses evicted")
XORCLE_STAT(search, Count, num_equivalent_clauses_at_input, "equivalent_clauses_at_input", "Equivalent clauses evicted at input")
XORCLE_STAT(search, Count, num_equivalent_clauses_at_subsumption, "equivalent_clauses_at_subsumption", "Equivalent clauses evicted at a subsumption")
XORCLE_STAT(search, Count, num_equivalent_clauses_at_substitution, "equivalent_clauses_at_substitution", "Equivalent clauses evicted at a substitution")
XORCLE_STAT(search, Count, num_equivalent_starting_learned_pairs, "equivalent_starting_learned_pairs", "Equivalent pairs of a starting and a learned clause")
XORCLE_STAT(search, Count, num_equivalent_learned_pairs, "equivalent_learned_pairs", "Equivalent pairs of learned clauses")
XORCLE_STAT(search, Count, peak_carrier_entries, "peak_carrier_entries", "Peak column-to-clause entries")

// --- Restarts, reported only when a schedule is in force --------------------

XORCLE_STAT(restart, Count, num_restarts, "restarts", "Restarts")
XORCLE_STAT(restart, Count, num_reused_levels, "reused_levels", "Reused levels")
XORCLE_STAT(restart, Count, num_canceled_restarts, "canceled_restarts", "Canceled restarts")
XORCLE_STAT(restart, Real, fast_lbd, "fast_lbd", "Fast LBD average")
XORCLE_STAT(restart, Real, slow_lbd, "slow_lbd", "Slow LBD average")

// --- The clause database ---------------------------------------------------

XORCLE_STAT(clauses, Count, learned_clauses, "learned_clauses", "Learned clauses")
XORCLE_STAT(clauses, Count, clauses_in_database, "clauses_in_database", "Clauses in database")
XORCLE_STAT(clauses, Count, clauses_allocated, "clauses_allocated", "Clauses allocated")
XORCLE_STAT(clauses, Real, average_learned_lbd, "average_learned_lbd", "Average learned LBD")
XORCLE_STAT(clauses, Count, learned_equation_bits, "learned_equation_bits", "Learned equation bits")
XORCLE_STAT(clauses, Count, learned_width_1, "learned_width_1", "Learned width-1 clauses")
XORCLE_STAT(clauses, Count, learned_width_2, "learned_width_2", "Learned width-2 clauses")
XORCLE_STAT(clauses, Count, subsume_narrowed_to_width_2, "subsume_narrowed_to_width_2", "Subsumption replacements of width at most 2")
XORCLE_STAT(clauses, Count, compaction_narrowed_to_width_2, "compaction_narrowed_to_width_2", "Compaction narrowings to width at most 2")

// --- Decisions from clause intersections, reported only at a depth above 1 --

XORCLE_STAT(intersection, Count, intersection_decisions, "intersection_decisions", "Decisions from an intersection")
XORCLE_STAT(intersection, Count, intersected_clauses, "intersected_clauses", "Clauses intersected below the top")
XORCLE_STAT(intersection, Count, intersection_dimension_before, "intersection_dimension_before", "Dimension before intersecting")
XORCLE_STAT(intersection, Count, intersection_dimension_after, "intersection_dimension_after", "Dimension after intersecting")
XORCLE_STAT(intersection, Count, intersection_empty, "intersection_empty", "Clauses with an empty intersection")
XORCLE_STAT(intersection, Count, intersection_skipped, "intersection_skipped", "Empty intersections found from the pivot columns alone")
XORCLE_STAT(intersection, Count, intersection_passed_satisfied_clauses, "intersection_passed_satisfied_clauses", "Satisfied clauses passed while intersecting")
XORCLE_STAT(intersection, Count, intersection_propagating_clauses, "intersection_propagating_clauses", "Propagating clauses found while intersecting")
XORCLE_STAT(intersection, Real, intersection_time_s, "intersection_time_s", "Intersection time (s), also in the decision time")

// --- Clause minimization, reported only when it ran -------------------------

XORCLE_STAT(minimize, Count, num_minimized_clauses, "minimized_clauses", "Minimized clauses")
XORCLE_STAT(minimize, Count, minimize_width_before, "minimize_width_before", "Width before minimizing")
XORCLE_STAT(minimize, Count, minimize_width_after, "minimize_width_after", "Width after minimizing")

// --- On-the-fly subsumption, reported only when it ran ----------------------

XORCLE_STAT(subsume, Count, num_subsumed_clauses, "subsumed_clauses", "Subsumed clauses")
XORCLE_STAT(subsume, Count, num_subsumed_conflicting_clauses, "subsumed_conflicting_clauses", "Subsumed conflicting clauses")
XORCLE_STAT(subsume, Count, num_subsume_replacements, "subsume_replacements", "Subsumption replacements")

// --- Clause deletion, reported only when it ran -----------------------------

XORCLE_STAT(deletion, Count, num_deletion_passes, "deletion_passes", "Deletion passes")
XORCLE_STAT(deletion, Count, num_deleted_clauses, "deleted_clauses", "Deleted clauses")

// --- Deferred propagation, reported only under a budget ---------------------

XORCLE_STAT(propagate_budget, Count, num_deferred_runs, "deferred_runs", "Deferred propagation runs")
XORCLE_STAT(propagate_budget, Count, num_out_of_order_learns, "out_of_order_learns", "Out-of-order learns")
XORCLE_STAT(propagate_budget, Count, num_undone_decisions, "undone_decisions", "Decisions undone by a missed propagation")
XORCLE_STAT(propagate_budget, Count, num_decision_propagations, "decision_propagations", "Decision-time propagations")
XORCLE_STAT(propagate_budget, Count, num_decision_conflicts, "decision_conflicts", "Decision-time conflicts")
XORCLE_STAT(propagate_budget, Real, mean_budget_fraction, "mean_budget_fraction", "Mean attempt budget (clause fraction)")
XORCLE_STAT(propagate_budget, Real, final_budget_fraction, "final_budget_fraction", "Final attempt budget (clause fraction)")

// --- The implication graph, reported only when it ran -----------------------

XORCLE_STAT(implication_graph, Count, implication_graph_runs, "implication_graph_runs", "Implication graph runs")
XORCLE_STAT(implication_graph, Count, implication_graph_edges_visited, "implication_graph_edges_visited", "Arcs the descendant searches visited")
XORCLE_STAT(implication_graph, Count, implication_graph_pairs_eligible, "implication_graph_pairs_eligible", "Vertex pairs with an out-arc on either side")
XORCLE_STAT(implication_graph, Count, implication_graph_pairs_examined, "implication_graph_pairs_examined", "Of those, the pairs the rule examined")
XORCLE_STAT(implication_graph, Count, implication_graph_refuted_sides, "implication_graph_refuted_sides", "Sides of the examined pairs whose span holds 0 = 1")
XORCLE_STAT(implication_graph, Count, implication_graph_runs_at_the_pair_cap, "implication_graph_runs_at_the_pair_cap", "Runs the pair cap stopped short")
XORCLE_STAT(implication_graph, Count, implication_graph_truncated_sides, "implication_graph_truncated_sides", "Sides the descendant cap truncated")
XORCLE_STAT(implication_graph, Count, implication_graph_units_fed, "implication_graph_units_fed", "Units fed to the root trail")
XORCLE_STAT(implication_graph, Count, implication_graph_runs_after_a_conflict, "implication_graph_runs_after_a_conflict", "Runs past the first conflict")
XORCLE_STAT(implication_graph, Count, implication_graph_runs_due_to_clauses, "implication_graph_runs_due_to_clauses", "Runs the clause interval made due")
XORCLE_STAT(implication_graph, Count, implication_graph_runs_due_to_conflicts, "implication_graph_runs_due_to_conflicts", "Runs the conflict interval made due")
XORCLE_STAT(implication_graph, Count, implication_graph_runs_due_to_a_feed, "implication_graph_runs_due_to_a_feed", "Runs an earlier run's units made due")
XORCLE_STAT(implication_graph, Count, implication_graph_follow_ups_suppressed, "implication_graph_follow_ups_suppressed", "Follow-up runs too few clauses reaching width at most 2 withheld")
XORCLE_STAT(implication_graph, Count, implication_graph_conflicts_waited, "implication_graph_conflicts_waited", "Conflicts spent waiting for the root trail")
XORCLE_STAT(implication_graph, Count, implication_graph_max_conflicts_waited, "implication_graph_max_conflicts_waited", "Longest wait for the root trail, in conflicts")
XORCLE_STAT(implication_graph, Count, implication_graph_width_2_clauses, "implication_graph_width_2_clauses", "Width-2 clauses in the graph")
XORCLE_STAT(implication_graph, Count, implication_graph_vertex_pairs, "implication_graph_vertex_pairs", "Vertex pairs in the graph, one per equation and its negation")
XORCLE_STAT(implication_graph, Real, implication_graph_span_intersection_time_s, "implication_graph_span_intersection_time_s", "Span-intersection time (s), also in the graph time")
XORCLE_STAT(implication_graph, Real, implication_graph_time_s, "implication_graph_time_s", "Implication graph time (s)")

// --- The exact watchlist, reported only when it selects ----------------------

XORCLE_STAT(exact_watchlist, Count, exact_touches, "exact_touches", "Cells swept")
XORCLE_STAT(exact_watchlist, Count, exact_skips, "exact_skips", "Cells left unswept, their pair unable to trigger")
XORCLE_STAT(exact_watchlist, Count, exact_fixups, "exact_fixups", "Second cells reduced by their first")
XORCLE_STAT(exact_watchlist, Count, exact_row_additions, "exact_row_additions", "Row additions in sweeps")
XORCLE_STAT(exact_watchlist, Count, exact_triggers, "exact_triggers", "Clauses queued")
XORCLE_STAT(exact_watchlist, Count, exact_saves, "exact_saves", "Cells saved")
XORCLE_STAT(exact_watchlist, Count, exact_restores, "exact_restores", "Cells restored")
XORCLE_STAT(exact_watchlist, Count, exact_rebuilds, "exact_rebuilds", "Cells rebuilt")
XORCLE_STAT(exact_watchlist, Count, exact_cleanups, "exact_cleanups", "Bucket cleanups")
XORCLE_STAT(exact_watchlist, Count, exact_cleanup_entries, "exact_cleanup_entries", "Bucket entries cleanups read")
XORCLE_STAT(exact_watchlist, Count, exact_cleanup_dropped, "exact_cleanup_dropped", "Stale bucket entries dropped")
XORCLE_STAT(exact_watchlist, Count, exact_peak_bucket_entries, "exact_peak_bucket_entries", "Peak bucket entries")
XORCLE_STAT(exact_watchlist, Count, exact_touches_ahead, "exact_touches_ahead", "Cells swept ahead of their sweep, as a trigger's partner")
XORCLE_STAT(exact_watchlist, Count, exact_sweeps_started, "exact_sweeps_started", "Sweeps started")
XORCLE_STAT(exact_watchlist, Count, exact_sweeps_finished, "exact_sweeps_finished", "Sweeps finished")
XORCLE_STAT(exact_watchlist, Count, exact_sweeps_dropped, "exact_sweeps_dropped", "Paused sweeps dropped at backjumps")

// --- Timing ----------------------------------------------------------------

XORCLE_STAT(timing, Real, parse_time_s, "parse_time_s", "Parse time (s)")
XORCLE_STAT(timing, Real, solve_time_s, "solve_time_s", "Solve time (s)")
XORCLE_STAT(timing, Real, decision_time_s, "decision_time_s", "Decision time (s), also in the solve time")

// --- The proof, reported only once post-processing has run ------------------
//
// The console block prints before post-processing, so that the verdict does not
// wait on it; the `c Proof:` line reports this time there instead. The CSV row
// is written afterwards and carries it as a column.

XORCLE_STAT(proof, Real, proof_postprocess_time_s, "proof_postprocess_time_s", "Proof post-processing time (s)")

#undef XORCLE_STAT
