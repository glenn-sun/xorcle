// -*- C++ -*-
//
/// @file options.inc
/// @brief The option table: one row per command-line option.
///
/// `src/cli/options.h` includes it once per macro.
///
/// - `XORCLE_OPTION(name, kind, default, lo, hi, help)` — a scalar option. The
///   command line spells `name` with dashes, so `compact_cadence` is
///   `--compact-cadence`. `kind` is one of the `OptionKind` tags: `Bool`, `U64`,
///   `Size`, `Real` (all four have inclusive bounds), `Text` (a free-form
///   string, e.g. a path) or `Name` (a string from a fixed vocabulary the
///   option checks for itself, in `validate_options`). The last two are
///   unbounded, so their `lo` and `hi` are empty.
/// - `XORCLE_ENUM_OPTION(name, type, default, help)` — an option whose value is
///   one of a fixed set of names.
/// - `XORCLE_ENUM_VALUE(option, spelling, enumerator)` — one accepted spelling
///   of one enum option. The **first** spelling naming a given enumerator is the
///   one the statistics output labels it with.
/// - `XORCLE_PRESET(spelling, config)` — a `--decisions=` spelling that also
///   fixes the CSIDS parameters.
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
#ifndef XORCLE_PRESET
#define XORCLE_PRESET(spelling, config)
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
XORCLE_OPTION(compact_cadence, Size, 16, 1, SIZE_MAX, "number of conflicts between compactions")

// --- Decisions -------------------------------------------------------------

XORCLE_ENUM_OPTION(decisions, DecisionProcedureName, DecisionProcedureName::csids,
                   "decision procedure (berkmin/cmtf are CSIDS presets; fixed_literals is a\n"
                   "debug baseline)")
XORCLE_ENUM_VALUE(decisions, "csids", DecisionProcedureName::csids)
XORCLE_ENUM_VALUE(decisions, "variable_vsids", DecisionProcedureName::variable_vsids)
XORCLE_ENUM_VALUE(decisions, "fixed_literals", DecisionProcedureName::fixed_literals)
XORCLE_ENUM_VALUE(decisions, "berkmin", DecisionProcedureName::csids)
XORCLE_ENUM_VALUE(decisions, "cmtf", DecisionProcedureName::csids)

XORCLE_PRESET(berkmin, csids_berkmin_config)
XORCLE_PRESET(cmtf, csids_cmtf_config)

XORCLE_OPTION(csids_initial, Real, 1.0, 0.0, INFINITY, "initial activity of asserting clauses")
XORCLE_OPTION(csids_bump, Real, 1.0, 0.0, INFINITY, "activity bump per resolution step")
XORCLE_OPTION(csids_decay, Real, 0.98, 0.0, 1.0, "activity decay per conflict")

XORCLE_OPTION(bump_responsible_clauses, Bool, false, false, true,
              "also bump the reason clauses of the learned clause's responsible trail\n"
              "elements")

XORCLE_ENUM_OPTION(phase, DecisionPhase, default_decision_phase,
                   "decide sampled equation (falsify) or its negation (satisfy)")
XORCLE_ENUM_VALUE(phase, "falsify", DecisionPhase::falsify)
XORCLE_ENUM_VALUE(phase, "satisfy", DecisionPhase::satisfy)
XORCLE_ENUM_VALUE(phase, "random", DecisionPhase::random)

// --- Selection -------------------------------------------------------------

XORCLE_ENUM_OPTION(selection, ClauseSelectorName, ClauseSelectorName::cyclic,
                   "which clause to try propagating from next")
XORCLE_ENUM_VALUE(selection, "cyclic", ClauseSelectorName::cyclic)
XORCLE_ENUM_VALUE(selection, "leading_term_watchlist", ClauseSelectorName::leading_term_watchlist)

XORCLE_OPTION(wait_for_queue, Size, 1, 1, SIZE_MAX,
              "for watchlists of earlier variables, do not queue until requested N times")

// --- Propagation -----------------------------------------------------------

XORCLE_OPTION(propagate_miss_rate, Real, 0.04, 0.0, 1.0,
              "target fraction of propagated equations learned out of order; 0 propagates\n"
              "to a fixed point, and so does the default under any other selector")

XORCLE_OPTION(propagate_window, Size, 2000, 1, SIZE_MAX,
              "learned equations over which the attempt budget decays by a factor of e\n"
              "when none is missed")

// --- Restarts --------------------------------------------------------------

XORCLE_ENUM_OPTION(restart, RestartMethod, RestartMethod::none, "restart schedule")
XORCLE_ENUM_VALUE(restart, "none", RestartMethod::none)
XORCLE_ENUM_VALUE(restart, "luby", RestartMethod::luby)
XORCLE_ENUM_VALUE(restart, "lbd", RestartMethod::lbd)

XORCLE_OPTION(restart_reuse_trail, Bool, false, false, true,
              "after a restart, decide with the same clauses as before, if possible "
              "(only with CSIDS)")

XORCLE_OPTION(restart_luby_unit, Size, 1, 1, SIZE_MAX,
              "conflicts for a luby value of 1 (required with --restart=luby)")
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

// --- Clause minimization ---------------------------------------------------

XORCLE_OPTION(minimize, Bool, true, false, true,
              "apply local minimization (self-subsumption) to each learned clause")

// --- On-the-fly subsumption -------------------------------------------------

XORCLE_OPTION(subsume, Bool, true, false, true,
              "replace the clauses the resolvents of conflict analysis subsume "
              "(only with cyclic selector)")

// --- Deletion --------------------------------------------------------------

XORCLE_OPTION(deletion, Bool, true, false, true,
              "periodically delete some learned clauses (only with cyclic selector)")
XORCLE_OPTION(deletion_cadence, Size, 100, 1, SIZE_MAX, "conflicts between deletion passes")
XORCLE_OPTION(deletion_core_lbd, Size, 2, 0, SIZE_MAX,
              "a learned clause of at most this LBD is never deleted")
XORCLE_OPTION(deletion_secondary_lbd, Size, 6, 0, SIZE_MAX,
              "a learned clause of at most this LBD survives until unused for "
              "--deletion-demote-cadence-in-passes deletion passes")
XORCLE_OPTION(deletion_demote_cadence_in_passes, Size, 2, 1, SIZE_MAX,
              "consecutive deletion passes a secondary-tier clause goes unused before it becomes local")

XORCLE_ENUM_OPTION(deletion_key, DeletionKey, DeletionKey::activity,
                   "which score orders a deletion pass (activity requires CSIDS)")
XORCLE_ENUM_VALUE(deletion_key, "lbd", DeletionKey::lbd)
XORCLE_ENUM_VALUE(deletion_key, "activity", DeletionKey::activity)

// --- The implication graph --------------------------------------------------

XORCLE_OPTION(implication_graph, Bool, false, false, true,
              "add to the root trail the equations that hold on both sides of a vertex of\n"
              "the implication graph of the width-2 clauses; needs --compact=true")
XORCLE_OPTION(implication_graph_clause_cadence, Size, 16, 1, SIZE_MAX,
              "clauses that became width at most 2 between implication graph runs")
XORCLE_OPTION(implication_graph_cadence, Size, 1000, 1, SIZE_MAX,
              "conflicts between implication graph runs, the fallback when too few\n"
              "clauses became width at most 2")
XORCLE_OPTION(implication_graph_max_pairs, Size, 10000, 1, SIZE_MAX,
              "vertex pairs one implication graph run examines, sampled uniformly from\n"
              "the pairs with an out-arc on either side")
XORCLE_OPTION(implication_graph_max_descendants, Size, 1000, 1, SIZE_MAX,
              "descendants one side's search keeps, whole distance levels first and a\n"
              "uniform sample of the level that would overrun this")

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
// are *parameters* of a subsystem; the switches that turn a subsystem on
// (`deletion`, `restart_reuse_trail`) are checked against their value rather
// than against having been given, in `validate_options`, because a `true`
// claims the feature just as loudly whether it is a default or explicit.

XORCLE_REQUIRES(compact_cadence, options.compact, "--compact=true")

XORCLE_REQUIRES(csids_initial, options.decisions == DecisionProcedureName::csids, "--decisions=csids")
XORCLE_REQUIRES(csids_bump, options.decisions == DecisionProcedureName::csids, "--decisions=csids")
XORCLE_REQUIRES(csids_decay, options.decisions == DecisionProcedureName::csids, "--decisions=csids")
XORCLE_REQUIRES(bump_responsible_clauses, options.decisions == DecisionProcedureName::csids,
                "--decisions=csids, the only procedure with an activity to bump")
XORCLE_REQUIRES(phase, options.decisions == DecisionProcedureName::csids,
                "--decisions=csids, the only procedure that decides a sampled equation")

XORCLE_REQUIRES(wait_for_queue, options.selection == ClauseSelectorName::leading_term_watchlist,
                "--selection=leading_term_watchlist")

// A rate of 0 is complete propagation, which every selector does, so only a
// nonzero one needs the selector that runs an attempt budget; the default has
// already resolved to 0 under any other.
XORCLE_REQUIRES(propagate_miss_rate,
                options.propagate_miss_rate == 0.0 ||
                    options.selection == ClauseSelectorName::cyclic,
                "--selection=cyclic")
XORCLE_REQUIRES(propagate_window, options.propagate_miss_rate != 0.0,
                "a nonzero --propagate-miss-rate")

XORCLE_REQUIRES(restart_luby_unit, options.restart == RestartMethod::luby, "--restart=luby")
XORCLE_REQUIRES(restart_lbd_fast, options.restart == RestartMethod::lbd, "--restart=lbd")
XORCLE_REQUIRES(restart_lbd_slow, options.restart == RestartMethod::lbd, "--restart=lbd")
XORCLE_REQUIRES(restart_lbd_min_ratio, options.restart == RestartMethod::lbd, "--restart=lbd")
XORCLE_REQUIRES(restart_lbd_min_conflicts, options.restart == RestartMethod::lbd, "--restart=lbd")
XORCLE_REQUIRES(restart_lbd_trail_ratio, options.restart == RestartMethod::lbd, "--restart=lbd")

XORCLE_REQUIRES(deletion_cadence, options.deletion, "--deletion=true")
XORCLE_REQUIRES(deletion_core_lbd, options.deletion, "--deletion=true")
XORCLE_REQUIRES(deletion_secondary_lbd, options.deletion, "--deletion=true")
XORCLE_REQUIRES(deletion_demote_cadence_in_passes, options.deletion, "--deletion=true")
XORCLE_REQUIRES(deletion_key, options.deletion, "--deletion=true")

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
#undef XORCLE_PRESET
#undef XORCLE_REQUIRES
