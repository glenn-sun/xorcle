// -*- C++ -*-
//
/// @file options.inc
/// @brief The option table: one row per command-line option.
///
/// `src/cli/options.h` includes it once per macro.
///
/// - `XORCLE_GROUP(name, condition, needs)` — a feature whose options are read
///   only when `condition` holds of the parsed `options`; given otherwise, one
///   is refused rather than silently ignored. `needs` completes the sentence
///   "`--option` requires ...". The group `always` is always read.
/// - `XORCLE_OPTION(group, name, kind, default, lo, hi, help)` — a scalar option
///   of `group`. The command line spells `name` with dashes, so
///   `deletion_eviction_rate` is `--deletion-eviction-rate`. `kind` is one of the
///   `OptionKind` tags: `Bool`, `U64`, `Size`, `Real` (all four have inclusive
///   bounds), `Text` (a free-form string, e.g. a path) or `Name` (a string from a
///   fixed vocabulary the option checks for itself, in `validate_options`). The
///   last two are unbounded, so their `lo` and `hi` are empty. Every default is a
///   constant.
/// - `XORCLE_ENUM_OPTION(group, name, type, default, help)` — an option whose
///   value is one of a fixed set of names.
/// - `XORCLE_ENUM_VALUE(option, spelling, enumerator)` — one accepted spelling
///   of one enum option. The **first** spelling naming a given enumerator is the
///   one the statistics output labels it with.
///
/// A switch that turns a feature on (`deletion`) is in the group `always`: it is
/// read whatever its value.

#ifndef XORCLE_GROUP
#define XORCLE_GROUP(name, condition, needs)
#endif
#ifndef XORCLE_OPTION
#define XORCLE_OPTION(group, name, kind, default_value, lo, hi, help)
#endif
#ifndef XORCLE_ENUM_OPTION
#define XORCLE_ENUM_OPTION(group, name, type, default_value, help)
#endif
#ifndef XORCLE_ENUM_VALUE
#define XORCLE_ENUM_VALUE(option, spelling, enumerator)
#endif

XORCLE_GROUP(always, true, "")

// --- Representation and reproducibility ------------------------------------

XORCLE_ENUM_OPTION(always, equation_repr, EquationRepr, EquationRepr::dense,
                   "representation for equations")
XORCLE_ENUM_VALUE(equation_repr, "dense", EquationRepr::dense)
XORCLE_ENUM_VALUE(equation_repr, "sparse", EquationRepr::sparse)

XORCLE_OPTION(always, seed, U64, 0, 0, UINT64_MAX, "RNG seed")

// --- Compaction ------------------------------------------------------------

XORCLE_OPTION(always, compact, Bool, true, false, true,
              "renumber the variables once those the root units dropped would shorten an\n"
              "equation by a word and 5% of its words")

// --- Decisions -------------------------------------------------------------

XORCLE_OPTION(always, csids_initial, Real, 1.0, 0.0, INFINITY,
              "initial activity of asserting clauses")
XORCLE_OPTION(always, csids_bump, Real, 1.0, 0.0, INFINITY, "activity bump per resolution step")
XORCLE_OPTION(always, csids_decay, Real, 0.98, 0.0, 1.0, "activity decay per conflict")
XORCLE_OPTION(always, csids_intersection_depth, Size, 50, 1, SIZE_MAX,
              "decide from the intersection, modulo the trail, of the top clause's linear\n"
              "negation with those of the open clauses below it, trying this many clauses\n"
              "in all (1: the top clause alone)")

XORCLE_ENUM_OPTION(always, csids_span, CsidsSpan, CsidsSpan::plain,
                   "intersect each clause's span(neg(C), U) (plain), or that span with 0 = 1\n"
                   "added (plus_minus), whose decisions are directions the trail does not\n"
                   "settle, the falsify side the one in the top clause's span")
XORCLE_ENUM_VALUE(csids_span, "plain", CsidsSpan::plain)
XORCLE_ENUM_VALUE(csids_span, "plus_minus", CsidsSpan::plus_minus)

XORCLE_OPTION(always, bump_responsible_clauses, Bool, false, false, true,
              "also bump the reason clauses of the learned clause's responsible trail\n"
              "elements")

XORCLE_ENUM_OPTION(always, phase, DecisionPhase, DecisionPhase::falsify,
                   "the side of a decision wherever no saved trail chooses it: the sampled\n"
                   "equation (falsify), its negation (satisfy), or either at random (random)")
XORCLE_ENUM_VALUE(phase, "falsify", DecisionPhase::falsify)
XORCLE_ENUM_VALUE(phase, "satisfy", DecisionPhase::satisfy)
XORCLE_ENUM_VALUE(phase, "random", DecisionPhase::random)

XORCLE_ENUM_OPTION(always, phase_saving, PhaseSaving, PhaseSaving::rotate,
                   "the saved trails that narrow a decision's space: the target trail, the\n"
                   "longest trail below a conflict level, then the last conflict's trail\n"
                   "(both), one of them (target-only, recent-only), neither (none), or both,\n"
                   "none, target-only and recent-only in turn as the conflicts go (rotate)")
XORCLE_ENUM_VALUE(phase_saving, "both", PhaseSaving::both)
XORCLE_ENUM_VALUE(phase_saving, "target-only", PhaseSaving::target_only)
XORCLE_ENUM_VALUE(phase_saving, "recent-only", PhaseSaving::recent_only)
XORCLE_ENUM_VALUE(phase_saving, "none", PhaseSaving::none)
XORCLE_ENUM_VALUE(phase_saving, "rotate", PhaseSaving::rotate)

XORCLE_GROUP(phase_target,
             options.phase_saving == PhaseSaving::both ||
                 options.phase_saving == PhaseSaving::target_only ||
                 options.phase_saving == PhaseSaving::rotate ||
                 (options.stabilize && (options.focused_phase_saving == PhaseSaving::both ||
                                        options.focused_phase_saving == PhaseSaving::target_only ||
                                        options.focused_phase_saving == PhaseSaving::rotate)),
             "--phase-saving=both, target-only or rotate, or one of them as --focused-phase-saving")
XORCLE_GROUP(phase_rotate,
             options.phase_saving == PhaseSaving::rotate ||
                 (options.stabilize && options.focused_phase_saving == PhaseSaving::rotate),
             "--phase-saving=rotate, or --stabilize=true with --focused-phase-saving=rotate")
XORCLE_OPTION(phase_target, phase_saving_target_reset, Size, 1000, 1, SIZE_MAX,
              "conflicts between resets of the target trail")
XORCLE_OPTION(phase_rotate, phase_rotate_interval, Size, 1000, 1, SIZE_MAX,
              "conflicts in each step of the rotation's first cycle; every step of cycle c\n"
              "lasts c times as many")

// --- Selection -------------------------------------------------------------

XORCLE_ENUM_OPTION(always, selection, ClauseSelectorName, ClauseSelectorName::exact_watchlist,
                   "which clause to try propagating from next")
XORCLE_ENUM_VALUE(selection, "cyclic", ClauseSelectorName::cyclic)
XORCLE_ENUM_VALUE(selection, "exact_watchlist", ClauseSelectorName::exact_watchlist)

// --- Propagation -----------------------------------------------------------

XORCLE_GROUP(cyclic, options.selection == ClauseSelectorName::cyclic, "--selection=cyclic")
XORCLE_GROUP(attempt_budget,
             options.selection == ClauseSelectorName::cyclic && options.propagate_miss_rate != 0.0,
             "--selection=cyclic and a nonzero --propagate-miss-rate")
XORCLE_OPTION(cyclic, propagate_miss_rate, Real, 0.05, 0.0, 1.0,
              "target fraction of propagated equations learned out of order; 0 propagates\n"
              "to a fixed point")

XORCLE_OPTION(attempt_budget, propagate_window, Size, 2000, 1, SIZE_MAX,
              "learned equations over which the attempt budget decays by a factor of e\n"
              "when none is missed")

// --- Restarts --------------------------------------------------------------

XORCLE_ENUM_OPTION(always, restart, RestartMethod, RestartMethod::none, "restart schedule")
XORCLE_ENUM_VALUE(restart, "none", RestartMethod::none)
XORCLE_ENUM_VALUE(restart, "luby", RestartMethod::luby)
XORCLE_ENUM_VALUE(restart, "lbd", RestartMethod::lbd)

XORCLE_GROUP(restart_luby, options.restart == RestartMethod::luby, "--restart=luby")
XORCLE_GROUP(restart_lbd, options.restart == RestartMethod::lbd, "--restart=lbd")
XORCLE_OPTION(restart_luby, restart_luby_unit, Size, 1, 1, SIZE_MAX,
              "conflicts for a luby value of 1")
XORCLE_OPTION(restart_lbd, restart_lbd_fast, Size, 33, 1, SIZE_MAX,
              "window of the fast LBD average")
XORCLE_OPTION(restart_lbd, restart_lbd_slow, Size, 1000, 1, SIZE_MAX,
              "window of the slow LBD average")
XORCLE_OPTION(restart_lbd, restart_lbd_min_ratio, Real, 1.1, 1.0, INFINITY,
              "fast/slow ratio needed for LBD restart")
XORCLE_OPTION(restart_lbd, restart_lbd_min_conflicts, Size, 1000, 1, SIZE_MAX,
              "elapsed conflicts needed for LBD restart")
XORCLE_OPTION(restart_lbd, restart_lbd_trail_ratio, Real, 1.4, 0.0, INFINITY,
              "block a restart while the trail exceeds this multiple of its recent\n"
              "average (0 never blocks)")

// --- Stable and focused modes ---------------------------------------------

XORCLE_OPTION(always, stabilize, Bool, false, false, true,
              "alternate a focused mode, which restarts by the LBD rule, decays activity\n"
              "by --focused-decay and saves phases by --focused-phase-saving, with stable\n"
              "mode, the search as configured; phases last 1, 4, 4, 9, 9, ... intervals")
XORCLE_GROUP(stabilize, options.stabilize, "--stabilize=true")
XORCLE_OPTION(stabilize, stabilize_interval, Size, 1000, 1, SIZE_MAX,
              "conflicts in the first focused phase")
XORCLE_OPTION(stabilize, focused_decay, Real, 0.5, 0.0, 1.0,
              "activity decay per conflict in focused mode")
XORCLE_ENUM_OPTION(stabilize, focused_phase_saving, PhaseSaving, PhaseSaving::recent_only,
                   "--phase-saving in focused mode")
XORCLE_ENUM_VALUE(focused_phase_saving, "both", PhaseSaving::both)
XORCLE_ENUM_VALUE(focused_phase_saving, "target-only", PhaseSaving::target_only)
XORCLE_ENUM_VALUE(focused_phase_saving, "recent-only", PhaseSaving::recent_only)
XORCLE_ENUM_VALUE(focused_phase_saving, "none", PhaseSaving::none)
XORCLE_ENUM_VALUE(focused_phase_saving, "rotate", PhaseSaving::rotate)
XORCLE_OPTION(stabilize, focused_restart_lbd_fast, Size, 33, 1, SIZE_MAX,
              "window of focused mode's fast LBD average")
XORCLE_OPTION(stabilize, focused_restart_lbd_slow, Size, 100000, 1, SIZE_MAX,
              "window of focused mode's slow LBD average")
XORCLE_OPTION(stabilize, focused_restart_lbd_min_ratio, Real, 1.1, 1.0, INFINITY,
              "fast/slow ratio needed for a focused-mode restart")
XORCLE_OPTION(stabilize, focused_restart_lbd_min_conflicts, Size, 2, 1, SIZE_MAX,
              "focused-mode conflicts needed between restarts")
XORCLE_OPTION(stabilize, focused_restart_lbd_trail_ratio, Real, 0.0, 0.0, INFINITY,
              "block a focused-mode restart while the trail exceeds this multiple of its\n"
              "recent average (0 never blocks)")

// --- Clause minimization ---------------------------------------------------

XORCLE_OPTION(always, minimize, Bool, true, false, true,
              "apply local minimization (self-subsumption) to each learned clause")

// --- On-the-fly subsumption -------------------------------------------------

XORCLE_OPTION(always, otf_subsume, Bool, true, false, true,
              "replace the clauses the resolvents of conflict analysis subsume")

// --- LBD -------------------------------------------------------------------

XORCLE_OPTION(always, lbd_window, Size, 2, 1, SIZE_MAX,
              "window of the average of the LBDs measured at a clause's uses as a reason")

// --- Deletion --------------------------------------------------------------

XORCLE_OPTION(always, deletion, Bool, true, false, true,
              "periodically delete some learned clauses")
XORCLE_GROUP(deletion, options.deletion, "--deletion=true")
XORCLE_OPTION(deletion, deletion_cadence_coefficient, Real, 0.25, 0.0, INFINITY,
              "conflicts between deletion passes are this times the square root of the "
              "conflicts so far plus --deletion-cadence-offset (positive)")
XORCLE_OPTION(deletion, deletion_cadence_offset, Size, 5000, 0, SIZE_MAX,
              "conflicts added to the conflicts so far under the square root of "
              "--deletion-cadence-coefficient")
XORCLE_OPTION(deletion, deletion_wait, Real, 6.0, 0.0, 62.0,
              "a learned clause may go unused for round(2^(wait - LBD)) consecutive deletion "
              "passes before it can be deleted")
XORCLE_OPTION(deletion, deletion_eviction_rate, Real, 0.5, 0.0, 1.0,
              "fraction of the deletion candidates a deletion pass evicts, worst first by "
              "--deletion-key")

XORCLE_ENUM_OPTION(deletion, deletion_key, DeletionKey, DeletionKey::activity,
                   "which score orders a deletion pass")
XORCLE_ENUM_VALUE(deletion_key, "lbd", DeletionKey::lbd)
XORCLE_ENUM_VALUE(deletion_key, "activity", DeletionKey::activity)

// --- BVE ---------------------------------------------------------------------

XORCLE_OPTION(always, bve, Bool, true, false, true,
              "bounded variable elimination before the first decision, of hyperplanes\n"
              "chosen by projection onto spans of the clauses")
XORCLE_GROUP(bve, options.bve, "--bve=true")
XORCLE_OPTION(bve, bve_group_cap, Size, 10, 1, 16,
              "the most rows a BVE group may hold")
XORCLE_OPTION(bve, bve_max_refusals_in_a_row, Size, 5, 1, SIZE_MAX,
              "BVE gives up a group after this many hyperplanes refused in a row")
XORCLE_OPTION(bve, bve_success_rate, Real, 0.01, 0.0, 1.0,
              "the least fraction of the clauses present at a BVE pass's start that it\n"
              "must remove for another pass to follow")
XORCLE_ENUM_OPTION(bve, bve_core_shrinking, BveCoreShrinking, BveCoreShrinking::none,
                   "how a BVE gate search shrinks its first core: keep it (none), or solve\n"
                   "again on the core until it stops shrinking (repeat)")
XORCLE_ENUM_VALUE(bve_core_shrinking, "none", BveCoreShrinking::none)
XORCLE_ENUM_VALUE(bve_core_shrinking, "repeat", BveCoreShrinking::repeat)
XORCLE_OPTION(bve, bve_gate_max_conflicts, Size, 100, 1, SIZE_MAX,
              "the most conflicts one solve of a BVE gate search may take; a first solve\n"
              "that reaches it finds no gate")
XORCLE_OPTION(bve, bve_gate_max_table_cost, Size, 500000, 0, SIZE_MAX,
              "the largest clauses^2 x max(1, 2^(rank - 6)) of a BVE gate search done by its\n"
              "truth table; a dearer one goes to the gate solver")
XORCLE_OPTION(bve, bve_gate_max_clause_share, Real, 0.1, 0.0, 1.0,
              "the largest share of the present clauses a BVE try may touch and still search\n"
              "for a gate; a try touching more resolves every pair")
XORCLE_OPTION(bve, bve_root_equations, Bool, true, false, true,
              "add the equations a one-sided BVE gate makes false at the root as unit clauses")

// --- The implication graph --------------------------------------------------

XORCLE_OPTION(always, implication_graph, Bool, true, false, true,
              "add to the root trail the equations that hold on both sides of a vertex of\n"
              "the implication graph of the width-2 clauses")
XORCLE_GROUP(implication_graph, options.implication_graph, "--implication-graph=true")
XORCLE_OPTION(implication_graph, implication_graph_cadence, Size, 1000, 1, SIZE_MAX,
              "conflicts between implication graph runs, a due run forcing a restart\n"
              "with replay")
XORCLE_OPTION(implication_graph, implication_graph_min_new_clauses, Size, 50, 0, SIZE_MAX,
              "width-2 clauses new since the last implication graph run below which a due\n"
              "run is skipped; 0 skips none")
XORCLE_OPTION(implication_graph, implication_graph_max_pairs, Size, 10000, 1, SIZE_MAX,
              "vertex pairs one implication graph run examines, sampled uniformly from\n"
              "the pairs with an out-arc on either side")
XORCLE_OPTION(implication_graph, implication_graph_max_descendants, Size, 100, 1, SIZE_MAX,
              "descendants one side's search keeps, whole distance levels first and a\n"
              "uniform sample of the level that would overrun this")

// --- Vivification ----------------------------------------------------------

XORCLE_OPTION(always, vivify, Bool, true, false, true,
              "vivification: at root visits, narrow clauses by assuming rows of their linear\n"
              "negation")
XORCLE_GROUP(vivify, options.vivify, "--vivify=true")
XORCLE_OPTION(vivify, vivify_rounds, Size, 2, 1, SIZE_MAX,
              "random bases a vivification pass tries on a clause, each on the clause as the\n"
              "last left it; a pass walks the clauses of width 3 and up in one uniform order\n"
              "and tries a clause of width w with probability 2^(3 - w)")
XORCLE_OPTION(vivify, vivify_cadence, Size, 1000, 0, SIZE_MAX,
              "conflicts after a vivification pass before the next may run")
XORCLE_OPTION(vivify, vivify_effort, Real, 0.01, 0.0, INFINITY,
              "rows a vivification pass may assume and imply, per row the search decided or\n"
              "implied since the last pass")

// --- The subsumption pass ---------------------------------------------------

XORCLE_OPTION(always, inproc_subsume_strengthen, Bool, false, false, true,
              "a subsumption and strengthening pass at root visits: a clause whose span\n"
              "holds another's is deleted, and one whose span plus 0 = 1 holds another's\n"
              "loses a row; a pass due above the root forces a restart with replay")
XORCLE_GROUP(inproc_subsume_strengthen, options.inproc_subsume_strengthen,
             "--inproc-subsume-strengthen=true")
XORCLE_OPTION(inproc_subsume_strengthen, inproc_subsume_strengthen_cadence, Size, 1000, 0,
              SIZE_MAX,
              "conflicts after a subsumption pass before the next may run")
XORCLE_OPTION(inproc_subsume_strengthen, inproc_subsume_strengthen_effort, Real, 10.0, 0.0,
              INFINITY,
              "rows a subsumption pass may read in its checks, per row the search decided\n"
              "or implied since the last pass; it visits the narrower clauses narrowest\n"
              "first")

// --- Output ----------------------------------------------------------------

XORCLE_OPTION(always, proof, Text, "", "", "", "write an LRUP(⊕) proof here")
XORCLE_OPTION(always, stats_file, Text, "", "", "",
              "append CSV statistics here instead of printing them")
XORCLE_GROUP(console, options.stats_file.empty(),
             "statistics on the console, which --stats-file replaces")
XORCLE_GROUP(report_columns, options.stats_file.empty() && options.report_cadence != 0,
             "statistics on the console, which --stats-file replaces, and --report-cadence=N "
             "with N above zero")
XORCLE_OPTION(console, report_cadence, Size, 100, 0, SIZE_MAX,
              "conflicts between progress report lines (0 = off)")
XORCLE_OPTION(report_columns, report, Name, "default", "", "",
              "progress columns (default/search/deletion/propagation/all, comma-joined)")

#undef XORCLE_OPTION
#undef XORCLE_ENUM_OPTION
#undef XORCLE_ENUM_VALUE
#undef XORCLE_GROUP
