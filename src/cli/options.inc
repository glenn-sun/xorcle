// -*- C++ -*-
//
/// @file options.inc
/// @brief The option table: one row per command-line option.
///
/// Including
/// it with the four macros below defined generates, from the same rows, the
/// options struct, the "was it given?" struct, the `--name=value` parser, the
/// bounds checks, the enum value tables and `--help`. `src/cli/options.h` does
/// each of those in turn; nothing else includes this file.
///
/// ## The macros
///
/// - `XORCLE_OPTION(name, kind, default, lo, hi, help)` — a scalar option.
///   `name` is the table name; the command line spells it with dashes, so
///   `compact_every` is `--compact-every`. `kind` is one of the `OptionKind`
///   tags: `Bool`, `U64`, `Size`, `Real` (bounds inclusive), `Ratio` (a double
///   with **exclusive** bounds) or `Text` (unbounded, so `lo` and `hi` are
///   empty).
/// - `XORCLE_ENUM_OPTION(name, type, default, help)` — an option whose value is
///   one of a fixed set of names.
/// - `XORCLE_ENUM_VALUE(option, spelling, enumerator)` — one accepted spelling
///   of one enum option. The **first** spelling naming a given enumerator is the
///   one the statistics output labels it with.
/// - `XORCLE_PRESET(spelling, config)` — a `--decisions=` spelling that fixes
///   the whole CSIDS triple.
///
/// Undefined macros expand to nothing, so an includer defines only the ones it
/// needs. All four are undefined again at the bottom.

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

// --- Storage and reproducibility -------------------------------------------

XORCLE_ENUM_OPTION(algebra, Algebra, Algebra::dense, "row storage backend")
XORCLE_ENUM_VALUE(algebra, "dense", Algebra::dense)
XORCLE_ENUM_VALUE(algebra, "sparse", Algebra::sparse)

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
XORCLE_OPTION(csids_decay, Ratio, 0.98, 0.0, 1.0, "alpha: activity decay per conflict")

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

// --- Restarts --------------------------------------------------------------
//
// Marked EXPERIMENTAL in `--help`: the paper's
// Conclusion lists restarts among the parts of Xorcle that are not yet fully
// implemented, and these schedules were never tuned against its benchmarks.

XORCLE_ENUM_OPTION(restart, RestartMethod, RestartMethod::none, "EXPERIMENTAL: restart schedule")
XORCLE_ENUM_VALUE(restart, "none", RestartMethod::none)
XORCLE_ENUM_VALUE(restart, "geometric", RestartMethod::geometric)
XORCLE_ENUM_VALUE(restart, "luby", RestartMethod::luby)

XORCLE_OPTION(restart_geometric_factor, Real, 1.5, 1.0, INFINITY,
              "EXPERIMENTAL: growth factor between geometric budgets")
XORCLE_OPTION(restart_initial, Size, 100, 1, SIZE_MAX,
              "EXPERIMENTAL: first geometric conflict budget")
XORCLE_OPTION(restart_luby_unit, Size, 100, 1, SIZE_MAX,
              "EXPERIMENTAL: conflict budget for a Luby value of 1")

// --- Output ----------------------------------------------------------------

XORCLE_OPTION(proof, Text, "", "", "", "write an LRUP(+) proof here; requires --compact=false")
XORCLE_OPTION(stats_file, Text, "", "", "", "append CSV statistics here instead of printing them")

#undef XORCLE_OPTION
#undef XORCLE_ENUM_OPTION
#undef XORCLE_ENUM_VALUE
#undef XORCLE_PRESET
