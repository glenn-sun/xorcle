// -*- C++ -*-
//
/// @file options.inc
/// @brief The option table: one row per command-line option.
///
/// `src/cli/options.h` includes it once per macro.
///
/// - `XORCLE_OPTION(name, kind, default, lo, hi, help)` — a scalar option. The
///   command line spells `name` with dashes, so `compact_every` is
///   `--compact-every`. `kind` is one of the `OptionKind` tags: `Bool`, `U64`,
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

XORCLE_OPTION(compact, Bool, true, false, true, "eliminate the leading terms of all known units")
XORCLE_OPTION(compact_every, Size, 16, 1, SIZE_MAX, "number of conflicts between compactions")

// --- Decisions -------------------------------------------------------------

XORCLE_ENUM_OPTION(decisions, DecisionProcedureName, DecisionProcedureName::csids,
                   "decision procedure (berkmin/cmtf are CSIDS presets)")
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

XORCLE_OPTION(bump_learned_clause_pivots, Bool, false, false, true,
              "also bump the clauses at the learned clause's pivots in the trail basis")

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

// --- Restarts --------------------------------------------------------------

XORCLE_ENUM_OPTION(restart, RestartMethod, RestartMethod::none, "restart schedule")
XORCLE_ENUM_VALUE(restart, "none", RestartMethod::none)
XORCLE_ENUM_VALUE(restart, "luby", RestartMethod::luby)
XORCLE_ENUM_VALUE(restart, "lbd", RestartMethod::lbd)

XORCLE_OPTION(restart_reuse_trail, Bool, true, false, true,
              "after a restart, decide with the same clauses as before, if possible "
              "(only with CSIDS)")

XORCLE_OPTION(restart_luby_unit, Size, 0, 0, SIZE_MAX,
              "multiplier for luby schedule")
XORCLE_OPTION(restart_lbd_fast, Size, 33, 1, SIZE_MAX,
              "window of the fast LBD average")
XORCLE_OPTION(restart_lbd_slow, Size, 100000, 1, SIZE_MAX,
              "window of the slow LBD average")
XORCLE_OPTION(restart_lbd_min_ratio, Real, 1.1, 1.0, INFINITY,
              "fast/slow ratio needed for LBD restart")
XORCLE_OPTION(restart_lbd_min_conflicts, Size, 200, 1, SIZE_MAX,
              "elapsed conflicts needed for LBD restart")
XORCLE_OPTION(restart_lbd_trail_ratio, Real, 1.4, 0.0, INFINITY,
              "do not restart if trail is this much longer than average (0 = off)")

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
XORCLE_OPTION(deletion_every, Size, 100, 1, SIZE_MAX, "conflicts between deletion passes")
XORCLE_OPTION(deletion_core_lbd, Size, 2, 0, SIZE_MAX,
              "a learned clause of at most this LBD is never deleted")
XORCLE_OPTION(deletion_tier2_lbd, Size, 6, 0, SIZE_MAX,
              "a learned clause of at most this LBD survives until unused for --deletion-tier2-demote-every conflicts")
XORCLE_OPTION(deletion_tier2_demote_every, Size, 200, 1, SIZE_MAX,
              "conflicts a tier2 clause goes unused before it becomes local")

XORCLE_ENUM_OPTION(deletion_key, DeletionKey, DeletionKey::activity,
                   "which score orders a deletion pass (activity requires CSIDS)")
XORCLE_ENUM_VALUE(deletion_key, "lbd", DeletionKey::lbd)
XORCLE_ENUM_VALUE(deletion_key, "activity", DeletionKey::activity)

// --- Output ----------------------------------------------------------------

XORCLE_OPTION(proof, Text, "", "", "", "write an LRUP(⊕) proof here")
XORCLE_OPTION(proof_raw, Text, "", "", "",
              "keep the raw proof here instead of in a temporary file")
XORCLE_OPTION(stats_file, Text, "", "", "", "append CSV statistics here instead of printing them")
XORCLE_OPTION(report_every, Size, 100, 0, SIZE_MAX,
              "conflicts between progress report lines (0 = off)")
XORCLE_OPTION(report, Name, "default", "", "",
              "progress columns (default/search/deletion/propagation/all, comma-joined)")

// --- What each option needs in order to be read -----------------------------
//
// An option named on a command line that would not read it is an error, so that
// no run reports a setting it did not act on. These rows cover the options that
// are *parameters* of a subsystem; the switches that turn a subsystem on
// (`deletion`, `restart_reuse_trail`) are checked against their value rather
// than against having been given, in `validate_options`, because a default
// `true` claims the feature just as loudly as an explicit one.

XORCLE_REQUIRES(compact_every, options.compact, "--compact=true")

XORCLE_REQUIRES(seed, options.decisions == DecisionProcedureName::csids,
                "--decisions=csids, the only procedure that draws on the RNG")
XORCLE_REQUIRES(csids_initial, options.decisions == DecisionProcedureName::csids, "--decisions=csids")
XORCLE_REQUIRES(csids_bump, options.decisions == DecisionProcedureName::csids, "--decisions=csids")
XORCLE_REQUIRES(csids_decay, options.decisions == DecisionProcedureName::csids, "--decisions=csids")
XORCLE_REQUIRES(bump_learned_clause_pivots, options.decisions == DecisionProcedureName::csids,
                "--decisions=csids, the only procedure with an activity to bump")
XORCLE_REQUIRES(phase, options.decisions == DecisionProcedureName::csids,
                "--decisions=csids, the only procedure that decides a sampled equation")

XORCLE_REQUIRES(wait_for_queue, options.selection == ClauseSelectorName::leading_term_watchlist,
                "--selection=leading_term_watchlist")

XORCLE_REQUIRES(restart_reuse_trail, options.restart != RestartMethod::none, "a restart schedule")
XORCLE_REQUIRES(restart_luby_unit, options.restart == RestartMethod::luby, "--restart=luby")
XORCLE_REQUIRES(restart_lbd_fast, options.restart == RestartMethod::lbd, "--restart=lbd")
XORCLE_REQUIRES(restart_lbd_slow, options.restart == RestartMethod::lbd, "--restart=lbd")
XORCLE_REQUIRES(restart_lbd_min_ratio, options.restart == RestartMethod::lbd, "--restart=lbd")
XORCLE_REQUIRES(restart_lbd_min_conflicts, options.restart == RestartMethod::lbd, "--restart=lbd")
XORCLE_REQUIRES(restart_lbd_trail_ratio, options.restart == RestartMethod::lbd, "--restart=lbd")

XORCLE_REQUIRES(deletion_every, options.deletion, "--deletion=true")
XORCLE_REQUIRES(deletion_core_lbd, options.deletion, "--deletion=true")
XORCLE_REQUIRES(deletion_tier2_lbd, options.deletion, "--deletion=true")
XORCLE_REQUIRES(deletion_tier2_demote_every, options.deletion, "--deletion=true")
XORCLE_REQUIRES(deletion_key, options.deletion, "--deletion=true")

XORCLE_REQUIRES(report_every, options.stats_file.empty(),
                "statistics on the console, which --stats-file replaces")
XORCLE_REQUIRES(report, options.stats_file.empty(),
                "statistics on the console, which --stats-file replaces")
XORCLE_REQUIRES(report, options.report_every != 0, "--report-every=N with N above zero")

#undef XORCLE_OPTION
#undef XORCLE_ENUM_OPTION
#undef XORCLE_ENUM_VALUE
#undef XORCLE_PRESET
#undef XORCLE_REQUIRES
