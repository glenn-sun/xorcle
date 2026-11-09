// -*- C++ -*-
//
/// @file options.inc
/// @brief The option table: one row per command-line option.
///
/// `src/cli/options.h` includes it once per macro.
///
/// - `XORCLE_OPTION(name, kind, default, lo, hi, help)` — a scalar option. The
///   command line spells `name` with dashes, so `deletion_lbd_window` is
///   `--deletion-lbd-window`. `kind` is one of the `OptionKind` tags: `Bool`, `U64`,
///   `Size`, `Real` (all four have inclusive bounds), `Text` (a free-form
///   string, e.g. a path) or `Name` (a string from a fixed vocabulary the
///   option checks for itself, in `validate_options`). The last two are
///   unbounded, so their `lo` and `hi` are empty.
/// - `XORCLE_ENUM_OPTION(name, type, default, help)` — an option whose value is
///   one of a fixed set of names.
/// - `XORCLE_ENUM_VALUE(option, spelling, enumerator)` — one accepted spelling
///   of one enum option. The **first** spelling naming a given enumerator is the
///   one the statistics output labels it with.
/// - `XORCLE_REQUIRES(option, condition, needs)` — `option` is only read when
///   `condition` holds of the parsed `options`, so giving it otherwise is
///   rejected rather than silently ignored. `needs` completes the sentence
///   "`--option` requires ...". An option may carry more than one row.

#ifndef XORCLE_OPTION
#define XORCLE_OPTION(name, kind, default_value, lo, hi, help)
#endif
#ifndef XORCLE_ENUM_OPTION
#define XORCLE_ENUM_OPTION(name, type, default_value, help)
#endif
#ifndef XORCLE_ENUM_VALUE
#define XORCLE_ENUM_VALUE(option, spelling, enumerator)
#endif
#ifndef XORCLE_REQUIRES
#define XORCLE_REQUIRES(option, condition, needs)
#endif

// --- Representation and reproducibility ------------------------------------

XORCLE_ENUM_OPTION(equation_repr, EquationRepr, EquationRepr::dense,
                   "representation for equations")
XORCLE_ENUM_VALUE(equation_repr, "dense", EquationRepr::dense)
XORCLE_ENUM_VALUE(equation_repr, "sparse", EquationRepr::sparse)

XORCLE_OPTION(seed, U64, 0, 0, UINT64_MAX, "RNG seed")

// --- Compaction ------------------------------------------------------------

XORCLE_OPTION(compact, Bool, true, false, true, "compaction: drop the leading variables of all known units and remove the\n"
              "clauses they satisfy")

// --- Decisions -------------------------------------------------------------

XORCLE_ENUM_OPTION(decisions, DecisionProcedureName, DecisionProcedureName::csids,
                   "decision procedure (fixed_literals is a debug baseline)")
XORCLE_ENUM_VALUE(decisions, "csids", DecisionProcedureName::csids)
XORCLE_ENUM_VALUE(decisions, "fixed_literals", DecisionProcedureName::fixed_literals)

XORCLE_OPTION(csids_initial, Real, 1.0, 0.0, INFINITY, "initial activity of asserting clauses")
XORCLE_OPTION(csids_bump, Real, 1.0, 0.0, INFINITY, "activity bump per resolution step")
XORCLE_OPTION(csids_decay, Real, 0.98, 0.0, 1.0, "activity decay per conflict")
XORCLE_OPTION(csids_intersection_depth, Size, 50, 1, SIZE_MAX,
              "decide from the intersection, modulo the trail, of the top clause's linear\n"
              "negation with those of the open clauses below it, trying this many clauses\n"
              "in all (1: the top clause alone)")

XORCLE_ENUM_OPTION(csids_span, CsidsSpan, CsidsSpan::plain,
                   "intersect each clause's span(neg(C), U) (plain), or that span with 0 = 1\n"
                   "added (plus_minus), whose decisions are directions the trail does not\n"
                   "settle, the falsify side the one in the top clause's span")
XORCLE_ENUM_VALUE(csids_span, "plain", CsidsSpan::plain)
XORCLE_ENUM_VALUE(csids_span, "plus_minus", CsidsSpan::plus_minus)

XORCLE_OPTION(phase_saving_recent, Size, 1, 0, SIZE_MAX, "recent conflict trails phase saving keeps")
XORCLE_OPTION(phase_saving_target, Bool, true, false, true,
              "also keep the target trail, the longest trail below a conflict level")
XORCLE_OPTION(phase_saving_target_reset, Size, 1000, 1, SIZE_MAX,
              "conflicts between resets of the target trail")
XORCLE_OPTION(phase_saving_target_on_ties, Bool, false, false, true,
              "replace the target trail by a trail as long as it, not only by a longer one")
XORCLE_OPTION(phase_rotate_interval, Size, 1000, 1, SIZE_MAX,
              "conflicts in each step of --phase=rotate's first cycle; every step of\n"
              "cycle c lasts c times as many")

XORCLE_OPTION(bump_responsible_clauses, Bool, false, false, true,
              "also bump the reason clauses of the learned clause's responsible trail\n"
              "elements")

XORCLE_ENUM_OPTION(phase, DecisionPhase, default_decision_phase,
                   "decide sampled equation (falsify) or its negation (satisfy), either at\n"
                   "random (random), or by the target trail and the recent conflict trails\n"
                   "(saved), or by those and the falsify side in an order that rotates with\n"
                   "the conflicts (rotate)")
XORCLE_ENUM_VALUE(phase, "falsify", DecisionPhase::falsify)
XORCLE_ENUM_VALUE(phase, "satisfy", DecisionPhase::satisfy)
XORCLE_ENUM_VALUE(phase, "random", DecisionPhase::random)
XORCLE_ENUM_VALUE(phase, "saved", DecisionPhase::saved)
XORCLE_ENUM_VALUE(phase, "rotate", DecisionPhase::rotate)

// --- Selection -------------------------------------------------------------

XORCLE_ENUM_OPTION(selection, ClauseSelectorName, ClauseSelectorName::exact_watchlist,
                   "which clause to try propagating from next")
XORCLE_ENUM_VALUE(selection, "cyclic", ClauseSelectorName::cyclic)
XORCLE_ENUM_VALUE(selection, "exact_watchlist", ClauseSelectorName::exact_watchlist)

// --- Propagation -----------------------------------------------------------

XORCLE_OPTION(propagate_miss_rate, Real, 0.05, 0.0, 1.0,
              "target fraction of propagated equations learned out of order; 0 propagates\n"
              "to a fixed point")

XORCLE_OPTION(propagate_window, Size, 2000, 1, SIZE_MAX,
              "learned equations over which the attempt budget decays by a factor of e\n"
              "when none is missed")

// --- Restarts --------------------------------------------------------------

XORCLE_ENUM_OPTION(restart, RestartMethod, RestartMethod::none, "restart schedule")
XORCLE_ENUM_VALUE(restart, "none", RestartMethod::none)
XORCLE_ENUM_VALUE(restart, "luby", RestartMethod::luby)
XORCLE_ENUM_VALUE(restart, "lbd", RestartMethod::lbd)

XORCLE_OPTION(restart_luby_unit, Size, 1, 1, SIZE_MAX,
              "conflicts for a luby value of 1")
XORCLE_OPTION(restart_lbd_fast, Size, 33, 1, SIZE_MAX,
              "window of the fast LBD average")
XORCLE_OPTION(restart_lbd_slow, Size, 1000, 1, SIZE_MAX,
              "window of the slow LBD average")
XORCLE_OPTION(restart_lbd_min_ratio, Real, 1.1, 1.0, INFINITY,
              "fast/slow ratio needed for LBD restart")
XORCLE_OPTION(restart_lbd_min_conflicts, Size, 1000, 1, SIZE_MAX,
              "elapsed conflicts needed for LBD restart")
XORCLE_OPTION(restart_lbd_trail_ratio, Real, 1.4, 0.0, INFINITY,
              "block a restart while the trail exceeds this multiple of its recent\n"
              "average (0 never blocks)")

// --- Stable and focused modes ---------------------------------------------

XORCLE_OPTION(stabilize, Bool, false, false, true,
              "alternate a focused mode, which restarts by the LBD rule, decays activity\n"
              "by --focused-decay and saves phases without the target trail, with stable\n"
              "mode, the search without restarts; phases last 1, 4, 4, 9, 9, ... intervals")
XORCLE_OPTION(stabilize_interval, Size, 1000, 1, SIZE_MAX,
              "conflicts in the first focused phase")
XORCLE_OPTION(focused_decay, Real, 0.5, 0.0, 1.0, "activity decay per conflict in focused mode")
XORCLE_OPTION(focused_restart_lbd_fast, Size, 33, 1, SIZE_MAX,
              "window of focused mode's fast LBD average")
XORCLE_OPTION(focused_restart_lbd_slow, Size, 100000, 1, SIZE_MAX,
              "window of focused mode's slow LBD average")
XORCLE_OPTION(focused_restart_lbd_min_ratio, Real, 1.1, 1.0, INFINITY,
              "fast/slow ratio needed for a focused-mode restart")
XORCLE_OPTION(focused_restart_lbd_min_conflicts, Size, 2, 1, SIZE_MAX,
              "focused-mode conflicts needed between restarts")
XORCLE_OPTION(focused_restart_lbd_trail_ratio, Real, 0.0, 0.0, INFINITY,
              "block a focused-mode restart while the trail exceeds this multiple of its\n"
              "recent average (0 never blocks)")

// --- Clause minimization ---------------------------------------------------

XORCLE_OPTION(minimize, Bool, true, false, true,
              "apply local minimization (self-subsumption) to each learned clause")

// --- On-the-fly subsumption -------------------------------------------------

XORCLE_OPTION(subsume, Bool, true, false, true,
              "replace the clauses the resolvents of conflict analysis subsume")

// --- Deletion --------------------------------------------------------------

XORCLE_OPTION(deletion, Bool, true, false, true,
              "periodically delete some learned clauses")
XORCLE_OPTION(deletion_cadence_coefficient, Real, 0.25, 0.0, INFINITY,
              "conflicts between deletion passes are this times the square root of the "
              "conflicts so far plus --deletion-cadence-offset (positive)")
XORCLE_OPTION(deletion_cadence_offset, Size, 5000, 0, SIZE_MAX,
              "conflicts added to the conflicts so far under the square root of "
              "--deletion-cadence-coefficient")
XORCLE_OPTION(deletion_wait, Real, 6.0, 0.0, 62.0,
              "a learned clause may go unused for round(2^(wait - LBD)) consecutive deletion "
              "passes before it can be deleted")
XORCLE_OPTION(deletion_lbd_window, Size, 2, 1, SIZE_MAX,
              "window of the average of the LBDs measured at a learned clause's uses as a reason")
XORCLE_OPTION(deletion_eviction_rate, Real, 0.5, 0.0, 1.0,
              "fraction of the deletion candidates a deletion pass evicts, worst first by "
              "--deletion-key")

XORCLE_ENUM_OPTION(deletion_key, DeletionKey, DeletionKey::activity,
                   "which score orders a deletion pass (activity requires CSIDS)")
XORCLE_ENUM_VALUE(deletion_key, "lbd", DeletionKey::lbd)
XORCLE_ENUM_VALUE(deletion_key, "activity", DeletionKey::activity)

// --- BVE ---------------------------------------------------------------------

XORCLE_OPTION(bve, Bool, true, false, true,
              "bounded variable elimination before the first decision, of hyperplanes\n"
              "chosen by projection onto spans of the clauses; needs --compact=true, and\n"
              "the default is off without it")
XORCLE_OPTION(bve_tail_group_cap, Size, 10, 1, 16,
              "the most tail rows a BVE tail group may hold")
XORCLE_OPTION(bve_try_from, Real, 10.0, 0.0, 1e9,
              "a BVE choice touching n clauses is tried with chance 2^(this - n), at most 1")
XORCLE_OPTION(bve_success_rate, Real, 0.01, 0.0, 1.0,
              "the least fraction of the clauses present at a BVE reading's start that it\n"
              "must remove for another reading to follow")

// --- The implication graph --------------------------------------------------

XORCLE_OPTION(implication_graph, Bool, true, false, true,
              "add to the root trail the equations that hold on both sides of a vertex of\n"
              "the implication graph of the width-2 clauses; needs --compact=true, and the\n"
              "default is off without it")
XORCLE_OPTION(implication_graph_clause_cadence, Size, 50, 1, SIZE_MAX,
              "clauses that became width at most 2 between implication graph runs")
XORCLE_OPTION(implication_graph_cadence, Size, 1000, 1, SIZE_MAX,
              "conflicts between implication graph runs, the fallback when too few\n"
              "clauses became width at most 2")
XORCLE_OPTION(implication_graph_max_pairs, Size, 10000, 1, SIZE_MAX,
              "vertex pairs one implication graph run examines, sampled uniformly from\n"
              "the pairs with an out-arc on either side")
XORCLE_OPTION(implication_graph_max_descendants, Size, 100, 1, SIZE_MAX,
              "descendants one side's search keeps, whole distance levels first and a\n"
              "uniform sample of the level that would overrun this")

// --- Vivification ----------------------------------------------------------

XORCLE_OPTION(vivify, Bool, true, false, true,
              "vivification: at root visits, narrow clauses by assuming rows of their linear\n"
              "negation; the default is off under --selection=cyclic with a nonzero\n"
              "--propagate-miss-rate")
XORCLE_OPTION(vivify_rounds, Size, 2, 1, SIZE_MAX,
              "times a vivification pass goes through the clauses of one width, from 3 up,\n"
              "in a fresh uniform order each time and trying a clause of width w with\n"
              "probability 2^(3 - w), before it takes the next width")
XORCLE_OPTION(vivify_cadence, Size, 1000, 0, SIZE_MAX,
              "conflicts after a vivification pass before the next may run")
XORCLE_OPTION(vivify_force_root_visit, Bool, true, false, true,
              "a forced root visit: a vivification pass due above the root goes to the\n"
              "root, and the decisions are replayed in their order afterwards")
XORCLE_OPTION(vivify_effort, Real, 0.01, 0.0, INFINITY,
              "rows a vivification pass may assume and imply, per row the search decided or\n"
              "implied since the last pass")

// --- The subsumption pass ---------------------------------------------------

XORCLE_OPTION(subsume_pass, Bool, false, false, true,
              "a subsumption and strengthening pass at root visits: a clause whose span\n"
              "holds another's is deleted, and one whose span plus 0 = 1 holds another's\n"
              "loses a row; a pass due above the root forces a visit to it")
XORCLE_OPTION(subsume_pass_cadence, Size, 1000, 0, SIZE_MAX,
              "conflicts after a subsumption pass before the next may run")
XORCLE_OPTION(subsume_pass_effort, Real, 10.0, 0.0, INFINITY,
              "rows a subsumption pass may read in its checks, per row the search decided\n"
              "or implied since the last pass; it visits the narrower clauses narrowest\n"
              "first")

// --- Output ----------------------------------------------------------------

XORCLE_OPTION(proof, Text, "", "", "", "write an LRUP(⊕) proof here")
XORCLE_OPTION(proof_raw, Text, "", "", "",
              "keep the intermediate proof here instead of in a temporary file")
XORCLE_OPTION(stats_file, Text, "", "", "", "append CSV statistics here instead of printing them")
XORCLE_OPTION(report_cadence, Size, 100, 0, SIZE_MAX,
              "conflicts between progress report lines (0 = off)")
XORCLE_OPTION(report, Name, "default", "", "",
              "progress columns (default/search/deletion/propagation/all, comma-joined)")

// --- What each option needs in order to be read -----------------------------
//
// An option named on a command line that would not read it is an error, so that
// no run reports a setting it did not act on. These rows cover the options that
// are *parameters* of a subsystem; a switch that turns a subsystem on
// (`deletion`) is checked against its value rather than against having been
// given, in `validate_options`, because a `true` claims the feature just as
// loudly whether it is a default or explicit.

XORCLE_REQUIRES(csids_initial, options.decisions == DecisionProcedureName::csids, "--decisions=csids")
XORCLE_REQUIRES(csids_bump, options.decisions == DecisionProcedureName::csids, "--decisions=csids")
XORCLE_REQUIRES(csids_decay, options.decisions == DecisionProcedureName::csids, "--decisions=csids")
XORCLE_REQUIRES(csids_intersection_depth, options.decisions == DecisionProcedureName::csids,
                "--decisions=csids")
XORCLE_REQUIRES(csids_span, options.decisions == DecisionProcedureName::csids, "--decisions=csids")
XORCLE_REQUIRES(phase_saving_recent, saves_trails(options.phase), "--phase=saved or rotate")
XORCLE_REQUIRES(phase_saving_target, saves_trails(options.phase), "--phase=saved or rotate")
XORCLE_REQUIRES(phase_saving_target_on_ties,
                saves_trails(options.phase) && options.phase_saving_target,
                "--phase=saved or rotate, and --phase-saving-target=true")
XORCLE_REQUIRES(phase_saving_target_reset,
                saves_trails(options.phase) && options.phase_saving_target,
                "--phase=saved or rotate, and --phase-saving-target=true")
XORCLE_REQUIRES(phase_rotate_interval, options.phase == DecisionPhase::rotate, "--phase=rotate")
XORCLE_REQUIRES(bump_responsible_clauses, options.decisions == DecisionProcedureName::csids,
                "--decisions=csids, the only procedure with an activity to bump")
XORCLE_REQUIRES(phase, options.decisions == DecisionProcedureName::csids,
                "--decisions=csids, the only procedure that decides a sampled equation")

XORCLE_REQUIRES(vivify_rounds, options.vivify, "--vivify")
XORCLE_REQUIRES(vivify_cadence, options.vivify, "--vivify")
XORCLE_REQUIRES(vivify_effort, options.vivify, "--vivify")
XORCLE_REQUIRES(vivify_force_root_visit, options.vivify, "--vivify")

XORCLE_REQUIRES(subsume_pass_cadence, options.subsume_pass, "--subsume-pass")
XORCLE_REQUIRES(subsume_pass_effort, options.subsume_pass, "--subsume-pass")

XORCLE_REQUIRES(propagate_miss_rate, options.selection == ClauseSelectorName::cyclic,
                "--selection=cyclic")
XORCLE_REQUIRES(propagate_window,
                options.selection == ClauseSelectorName::cyclic &&
                    options.propagate_miss_rate != 0.0,
                "--selection=cyclic and a nonzero --propagate-miss-rate")

XORCLE_REQUIRES(restart_luby_unit, options.restart == RestartMethod::luby, "--restart=luby")
XORCLE_REQUIRES(restart_lbd_fast, options.restart == RestartMethod::lbd, "--restart=lbd")
XORCLE_REQUIRES(restart_lbd_slow, options.restart == RestartMethod::lbd, "--restart=lbd")
XORCLE_REQUIRES(restart_lbd_min_ratio, options.restart == RestartMethod::lbd, "--restart=lbd")
XORCLE_REQUIRES(restart_lbd_min_conflicts, options.restart == RestartMethod::lbd, "--restart=lbd")
XORCLE_REQUIRES(restart_lbd_trail_ratio, options.restart == RestartMethod::lbd, "--restart=lbd")

XORCLE_REQUIRES(stabilize_interval, options.stabilize, "--stabilize=true")
XORCLE_REQUIRES(focused_decay, options.stabilize, "--stabilize=true")
XORCLE_REQUIRES(focused_restart_lbd_fast, options.stabilize, "--stabilize=true")
XORCLE_REQUIRES(focused_restart_lbd_slow, options.stabilize, "--stabilize=true")
XORCLE_REQUIRES(focused_restart_lbd_min_ratio, options.stabilize, "--stabilize=true")
XORCLE_REQUIRES(focused_restart_lbd_min_conflicts, options.stabilize, "--stabilize=true")
XORCLE_REQUIRES(focused_restart_lbd_trail_ratio, options.stabilize, "--stabilize=true")

XORCLE_REQUIRES(deletion_cadence_coefficient, options.deletion, "--deletion=true")
XORCLE_REQUIRES(deletion_cadence_offset, options.deletion, "--deletion=true")
XORCLE_REQUIRES(deletion_wait, options.deletion, "--deletion=true")
XORCLE_REQUIRES(deletion_lbd_window, options.deletion, "--deletion=true")
XORCLE_REQUIRES(deletion_eviction_rate, options.deletion, "--deletion=true")
XORCLE_REQUIRES(deletion_key, options.deletion, "--deletion=true")

XORCLE_REQUIRES(bve_tail_group_cap, options.bve, "--bve=true")
XORCLE_REQUIRES(bve_try_from, options.bve, "--bve=true")
XORCLE_REQUIRES(bve_success_rate, options.bve, "--bve=true")

XORCLE_REQUIRES(implication_graph_clause_cadence, options.implication_graph,
                "--implication-graph=true")
XORCLE_REQUIRES(implication_graph_cadence, options.implication_graph, "--implication-graph=true")
XORCLE_REQUIRES(implication_graph_max_pairs, options.implication_graph, "--implication-graph=true")
XORCLE_REQUIRES(implication_graph_max_descendants, options.implication_graph,
                "--implication-graph=true")

XORCLE_REQUIRES(report_cadence, options.stats_file.empty(),
                "statistics on the console, which --stats-file replaces")
XORCLE_REQUIRES(report, options.stats_file.empty(),
                "statistics on the console, which --stats-file replaces")
XORCLE_REQUIRES(report, options.report_cadence != 0, "--report-cadence=N with N above zero")

#undef XORCLE_OPTION
#undef XORCLE_ENUM_OPTION
#undef XORCLE_ENUM_VALUE
#undef XORCLE_REQUIRES
