// -*- C++ -*-
//
/// @file stats_fields.inc
/// @brief The statistics table: one row per numeric field of a run record.
///
/// `src/util/stats.h` includes it to declare `RunStats`'s members, and
/// `src/util/stats_writer.cpp` includes it to write both output formats, so a
/// row added here reaches the console block and the CSV together and the two
/// can no longer drift.
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

// --- Restarts, reported only when a schedule is in force --------------------

XORCLE_STAT(restart, Count, num_restarts, "restarts", "Restarts")
XORCLE_STAT(restart, Count, num_reused_levels, "reused_levels", "Reused levels")
XORCLE_STAT(restart, Count, num_blocked_restarts, "blocked_restarts", "Blocked restarts")
XORCLE_STAT(restart, Real, fast_lbd, "fast_lbd", "Fast LBD average")
XORCLE_STAT(restart, Real, slow_lbd, "slow_lbd", "Slow LBD average")

// --- The clause database ---------------------------------------------------

XORCLE_STAT(clauses, Count, learned_clauses, "learned_clauses", "Learned clauses")
XORCLE_STAT(clauses, Count, clauses_in_database, "clauses_in_database", "Clauses in database")
XORCLE_STAT(clauses, Count, clauses_allocated, "clauses_allocated", "Clauses allocated")
XORCLE_STAT(clauses, Real, average_learned_lbd, "average_learned_lbd", "Average learned LBD")

// --- Clause minimization, reported only when it ran -------------------------

XORCLE_STAT(minimize, Count, num_minimized_clauses, "minimized_clauses", "Minimized clauses")
XORCLE_STAT(minimize, Count, minimize_width_before, "minimize_width_before", "Width before minimizing")
XORCLE_STAT(minimize, Count, minimize_width_after, "minimize_width_after", "Width after minimizing")

// --- Clause deletion, reported only when it ran -----------------------------

XORCLE_STAT(deletion, Count, num_deletion_passes, "deletion_passes", "Deletion passes")
XORCLE_STAT(deletion, Count, num_deleted_clauses, "deleted_clauses", "Deleted clauses")
XORCLE_STAT(deletion, Count, num_demoted_clauses, "demoted_clauses", "Tier demotions")
XORCLE_STAT(deletion, Count, num_promoted_clauses, "promoted_clauses", "Tier promotions")

// --- Timing ----------------------------------------------------------------

XORCLE_STAT(timing, Real, parse_time_s, "parse_time_s", "Parse time (s)")
XORCLE_STAT(timing, Real, solve_time_s, "solve_time_s", "Solve time (s)")

// --- The proof, reported only once post-processing has run ------------------
//
// The console block prints before post-processing, so that the verdict does not
// wait on it; the `c Proof:` line reports this time there instead. The CSV row
// is written afterwards and carries it as a column.

XORCLE_STAT(proof, Real, proof_postprocess_time_s, "proof_postprocess_time_s", "Proof post-processing time (s)")

#undef XORCLE_STAT
