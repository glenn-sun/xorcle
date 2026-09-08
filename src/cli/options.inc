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
///   `Size`, `Real` (all four have inclusive bounds) or `Text` (unbounded, so
///   `lo` and `hi` are empty).
/// - `XORCLE_ENUM_OPTION(name, type, default, help)` — an option whose value is
///   one of a fixed set of names.
/// - `XORCLE_ENUM_VALUE(option, spelling, enumerator)` — one accepted spelling
///   of one enum option. The **first** spelling naming a given enumerator is the
///   one the statistics output labels it with.
/// - `XORCLE_PRESET(spelling, config)` — a `--decisions=` spelling that also
///   fixes the CSIDS parameters.

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

// --- Representation and reproducibility ------------------------------------

XORCLE_ENUM_OPTION(equation_repr, EquationRepr, EquationRepr::dense,
                   "representation for equations")
XORCLE_ENUM_VALUE(equation_repr, "dense", EquationRepr::dense)
XORCLE_ENUM_VALUE(equation_repr, "sparse", EquationRepr::sparse)

XORCLE_OPTION(seed, U64, 0, 0, UINT64_MAX, "base RNG seed; a run is reproducible from it")

// --- Compaction ------------------------------------------------------------

XORCLE_OPTION(compact, Bool, true, false, true, "variable compaction")
XORCLE_OPTION(compact_every, Size, 16, 1, SIZE_MAX, "conflicts between compactions")

// --- Decisions -------------------------------------------------------------

XORCLE_ENUM_OPTION(decisions, DecisionProcedureName, DecisionProcedureName::csids,
                   "decision procedure; berkmin and cmtf are CSIDS presets")
XORCLE_ENUM_VALUE(decisions, "csids", DecisionProcedureName::csids)
XORCLE_ENUM_VALUE(decisions, "variable_vsids", DecisionProcedureName::variable_vsids)
XORCLE_ENUM_VALUE(decisions, "fixed_literals", DecisionProcedureName::fixed_literals)
XORCLE_ENUM_VALUE(decisions, "berkmin", DecisionProcedureName::csids)
XORCLE_ENUM_VALUE(decisions, "cmtf", DecisionProcedureName::csids)

XORCLE_PRESET(berkmin, csids_berkmin_config)
XORCLE_PRESET(cmtf, csids_cmtf_config)

XORCLE_OPTION(csids_initial, Real, 1.0, 0.0, INFINITY, "A0: activity a new asserting clause enters at")
XORCLE_OPTION(csids_bump, Real, 1.0, 0.0, INFINITY, "delta: activity bump per resolution step")
XORCLE_OPTION(csids_decay, Real, 0.98, 0.0, 1.0, "alpha: activity decay per conflict")

XORCLE_ENUM_OPTION(phase, DecisionPhase, default_decision_phase,
                   "which side of a sampled clause row to decide")
XORCLE_ENUM_VALUE(phase, "falsify", DecisionPhase::falsify)
XORCLE_ENUM_VALUE(phase, "satisfy", DecisionPhase::satisfy)
XORCLE_ENUM_VALUE(phase, "random", DecisionPhase::random)

// --- Selection -------------------------------------------------------------

XORCLE_ENUM_OPTION(selection, ClauseSelectorName, ClauseSelectorName::cyclic,
                   "which clause to try propagating from next")
XORCLE_ENUM_VALUE(selection, "cyclic", ClauseSelectorName::cyclic)
XORCLE_ENUM_VALUE(selection, "leading_term_watchlist", ClauseSelectorName::leading_term_watchlist)

XORCLE_OPTION(defer_pushes, Size, 1, 1, SIZE_MAX,
              "bottom pushes before a leading-term-watchlist clause is queued")

// --- Restarts --------------------------------------------------------------

XORCLE_ENUM_OPTION(restart, RestartMethod, RestartMethod::none, "restart schedule")
XORCLE_ENUM_VALUE(restart, "none", RestartMethod::none)
XORCLE_ENUM_VALUE(restart, "luby", RestartMethod::luby)
XORCLE_ENUM_VALUE(restart, "lbd", RestartMethod::lbd)

XORCLE_OPTION(restart_reuse_trail, Bool, true, false, true,
              "a restart keeps the decisions it would re-make at once")

XORCLE_OPTION(restart_luby_unit, Size, 0, 0, SIZE_MAX,
              "conflict budget for a Luby value of 1; --restart=luby requires it")
XORCLE_OPTION(restart_lbd_fast, Size, 33, 1, SIZE_MAX,
              "window of the fast LBD average")
XORCLE_OPTION(restart_lbd_slow, Size, 100000, 1, SIZE_MAX,
              "window of the slow LBD average")
XORCLE_OPTION(restart_lbd_margin, Real, 1.1, 1.0, INFINITY,
              "how far the fast LBD average must exceed the slow one")
XORCLE_OPTION(restart_lbd_min, Size, 2, 1, SIZE_MAX,
              "conflicts an LBD restart waits out")
XORCLE_OPTION(restart_lbd_block, Real, 1.4, 0.0, INFINITY,
              "trail length, relative to average, that blocks an LBD restart; 0 never blocks")

// --- Clause minimization ---------------------------------------------------

XORCLE_OPTION(minimize, Bool, true, false, true,
              "shrink each learned clause by resolving past the asserting point")

// --- Deletion --------------------------------------------------------------

XORCLE_OPTION(deletion, Bool, true, false, true,
              "delete the least useful learned clauses; yields to an incompatible --decisions/--selection")
XORCLE_OPTION(deletion_every, Size, 200, 1, SIZE_MAX, "conflicts between deletion passes")
XORCLE_OPTION(deletion_core_lbd, Size, 2, 0, SIZE_MAX,
              "a learned clause of at most this LBD is never deleted")
XORCLE_OPTION(deletion_tier2_lbd, Size, 6, 0, SIZE_MAX,
              "a learned clause of at most this LBD survives until two deletion passes go unused")

XORCLE_ENUM_OPTION(deletion_key, DeletionKey, DeletionKey::activity,
                   "which score orders the clauses a deletion pass deletes")
XORCLE_ENUM_VALUE(deletion_key, "lbd", DeletionKey::lbd)
XORCLE_ENUM_VALUE(deletion_key, "activity", DeletionKey::activity)

// --- Output ----------------------------------------------------------------

XORCLE_OPTION(proof, Text, "", "", "", "write an LRUP(⊕) proof here")
XORCLE_OPTION(proof_raw, Text, "", "", "",
              "keep the raw proof here instead of in a temporary file")
XORCLE_OPTION(stats_file, Text, "", "", "", "append CSV statistics here instead of printing them")
XORCLE_OPTION(report_every, Size, 100, 0, SIZE_MAX,
              "conflicts between progress report lines; 0 reports nothing")

#undef XORCLE_OPTION
#undef XORCLE_ENUM_OPTION
#undef XORCLE_ENUM_VALUE
#undef XORCLE_PRESET
