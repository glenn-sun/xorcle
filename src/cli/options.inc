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
              "renumber variables to drop columns that are all-zero")

// --- Decisions -------------------------------------------------------------

XORCLE_OPTION(always, csids_initial, Real, 1.0, 0.0, INFINITY,
              "initial activity of asserting clauses")
XORCLE_OPTION(always, csids_bump, Real, 1.0, 0.0, INFINITY,
              "activity bump per use in conflict analysis")
XORCLE_OPTION(always, csids_decay, Real, 0.98, 0.0, 1.0, "activity decay per conflict")
XORCLE_OPTION(always, csids_intersection_depth, Size, 50, 1, SIZE_MAX,
              "intersect top N clauses by activity before sampling (1: no intersection)")

XORCLE_OPTION(always, csids_intersection_allow_negation, Bool, false, false, true,
              "make intersection agnostic to the polarity of equations")

XORCLE_OPTION(always, csids_bump_learned_positions, Bool, false, false, true,
              "bump the reason clauses at the learned clause's trail positions")

XORCLE_ENUM_OPTION(always, phase_default, DecisionPhase, DecisionPhase::falsify,
                   "in decisions, falsify a clause's equation (towards unit propagation), "
                   "satisfy the equation (and the clause), or choose randomly")
XORCLE_ENUM_VALUE(phase_default, "falsify", DecisionPhase::falsify)
XORCLE_ENUM_VALUE(phase_default, "satisfy", DecisionPhase::satisfy)
XORCLE_ENUM_VALUE(phase_default, "random", DecisionPhase::random)

XORCLE_ENUM_OPTION(always, phase_saving, PhaseSaving, PhaseSaving::rotate,
                   "which saved trails to use for deciding the phase of a sampled equation")
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
             "--phase-saving=both|target-only|rotate or "
             "--focused-phase-saving=both|target-only|rotate")
XORCLE_GROUP(phase_rotate,
             options.phase_saving == PhaseSaving::rotate ||
                 (options.stabilize && options.focused_phase_saving == PhaseSaving::rotate),
             "--phase-saving=rotate or --focused-phase-saving=rotate")
XORCLE_OPTION(phase_target, phase_saving_target_reset, Size, 1000, 1, SIZE_MAX,
              "conflicts between resets of the target trail")
XORCLE_OPTION(phase_rotate, phase_rotate_starting_interval, Size, 1000, 1, SIZE_MAX,
              "starting number of conflicts for phase rotation steps")

// --- Selection -------------------------------------------------------------

XORCLE_ENUM_OPTION(always, selection, ClauseSelectorName, ClauseSelectorName::exact_watchlist,
                   "method to pick which clause to try propagating from next")
XORCLE_ENUM_VALUE(selection, "cyclic", ClauseSelectorName::cyclic)
XORCLE_ENUM_VALUE(selection, "exact-watchlist", ClauseSelectorName::exact_watchlist)

// --- Propagation -----------------------------------------------------------

XORCLE_GROUP(cyclic, options.selection == ClauseSelectorName::cyclic, "--selection=cyclic")
XORCLE_OPTION(cyclic, propagate_miss_rate, Real, 0.05, 0.0, 1.0,
              "target fraction of propagated equations learned out of order")

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
              "alternate between stable mode (standard config) and focused mode (customize "
              "with --focused-restart-lbd-*, --focused-decay, and --focused-phase-saving)")
XORCLE_GROUP(stabilize, options.stabilize, "--stabilize=true")
XORCLE_OPTION(stabilize, stabilize_interval, Size, 1000, 1, SIZE_MAX,
              "conflicts in the first focused phase")
XORCLE_OPTION(stabilize, focused_decay, Real, 0.5, 0.0, 1.0,
              "activity decay per conflict in focused mode")
XORCLE_ENUM_OPTION(stabilize, focused_phase_saving, PhaseSaving, PhaseSaving::recent_only,
                   "phase saving preference in focused mode")
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
              "replace the clauses that the resolvents of conflict analysis subsume")

// --- LBD -------------------------------------------------------------------

XORCLE_OPTION(always, lbd_window, Size, 2, 1, SIZE_MAX,
              "window of each clause's recent average LBD")

// --- Deletion --------------------------------------------------------------

XORCLE_OPTION(always, deletion, Bool, true, false, true,
              "periodically delete some learned clauses")
XORCLE_GROUP(deletion, options.deletion, "--deletion=true")
XORCLE_OPTION(deletion, deletion_cadence_coefficient, Real, 0.25, 0.0, INFINITY,
              "at conflict M, wait X * sqrt(M + --deletion-cadence-offset) conflicts before "
              "the next deletion pass (must be positive)")
XORCLE_OPTION(deletion, deletion_cadence_offset, Size, 5000, 0, SIZE_MAX,
              "at conflict M, wait --deletion-cadence-coefficient * sqrt(M + N) conflicts "
              "before the next deletion pass")
XORCLE_OPTION(deletion, deletion_wait, Real, 6.0, 0.0, 62.0,
              "a learned clause may go unused for round(2^(X - LBD)) consecutive deletion "
              "passes before it can be deleted")
XORCLE_OPTION(deletion, deletion_eviction_rate, Real, 0.5, 0.0, 1.0,
              "fraction of the deletion candidates that a deletion pass evicts")

XORCLE_ENUM_OPTION(deletion, deletion_key, DeletionKey, DeletionKey::activity,
                   "which score orders a deletion pass")
XORCLE_ENUM_VALUE(deletion_key, "lbd", DeletionKey::lbd)
XORCLE_ENUM_VALUE(deletion_key, "activity", DeletionKey::activity)

// --- BVE ---------------------------------------------------------------------

XORCLE_OPTION(always, bve, Bool, true, false, true, "bounded projection as preprocessing")
XORCLE_GROUP(bve, options.bve, "--bve=true")
XORCLE_OPTION(bve, bve_residual_dimensions, Size, 10, 1, 16,
              "during BVE's search for appropriate hyperplanes, max dimensions outside core "
              "for a clause group")
XORCLE_OPTION(bve, bve_max_refusals_in_a_row, Size, 5, 1, SIZE_MAX,
              "BVE gives up a group after this many hyperplanes refused in a row")
XORCLE_OPTION(bve, bve_success_rate, Real, 0.01, 0.0, 1.0,
              "stop BVE after a pass that removes less than this fraction of its clauses")
XORCLE_OPTION(bve, bve_gate_max_conflicts, Size, 100, 1, SIZE_MAX,
              "maximum number of conflicts for BVE gate search")
XORCLE_OPTION(bve, bve_gate_units, Bool, true, false, true,
              "allow one-sided BVE gates to add units")

// --- The implication graph --------------------------------------------------

XORCLE_OPTION(always, implication_graph, Bool, true, false, true,
              "form the implication graph of width-2 clauses to learn new units")
XORCLE_GROUP(implication_graph, options.implication_graph, "--implication-graph=true")
XORCLE_OPTION(implication_graph, implication_graph_cadence, Size, 1000, 1, SIZE_MAX,
              "conflicts between implication graph runs")
XORCLE_OPTION(implication_graph, implication_graph_min_new_clauses, Size, 50, 0, SIZE_MAX,
              "skip implication graph run if fewer than this many new width-2 clauses have "
              "been learned")
XORCLE_OPTION(implication_graph, implication_graph_max_pairs, Size, 10000, 1, SIZE_MAX,
              "number of vertex pairs that one implication graph run searches")
XORCLE_OPTION(implication_graph, implication_graph_max_descendants, Size, 100, 1, SIZE_MAX,
              "number of descendants searched in implication graph")

// --- Vivification ----------------------------------------------------------

XORCLE_OPTION(always, vivify, Bool, true, false, true,
              "narrow clauses by unit propagation from subspaces of their linear negation")
XORCLE_GROUP(vivify, options.vivify, "--vivify=true")
XORCLE_OPTION(vivify, vivify_bases, Size, 2, 1, SIZE_MAX,
              "number of different random bases tried on a clause during vivification")
XORCLE_OPTION(vivify, vivify_cadence, Size, 1000, 0, SIZE_MAX,
              "conflicts between vivification passes")
XORCLE_OPTION(vivify, vivify_effort, Real, 0.01, 0.0, INFINITY,
              "vivification effort relative to amount of main search work")

// --- Inprocessing subsumption and strengthening ----------------------------

XORCLE_OPTION(always, inproc_subsume_strengthen, Bool, false, false, true,
              "detect subsumption and strengthening as inprocessing")
XORCLE_GROUP(inproc_subsume_strengthen, options.inproc_subsume_strengthen,
             "--inproc-subsume-strengthen=true")
XORCLE_OPTION(inproc_subsume_strengthen, inproc_subsume_strengthen_cadence, Size, 1000, 0,
              SIZE_MAX,
              "conflicts between subsumption/strengthening passes")
XORCLE_OPTION(inproc_subsume_strengthen, inproc_subsume_strengthen_effort, Real, 10.0, 0.0,
              INFINITY,
              "subsumption/strengthening effort relative to amount of main search work")

// --- Output ----------------------------------------------------------------

XORCLE_OPTION(always, proof, Text, "", "", "", "output path for LRUP(⊕) proof")
XORCLE_GROUP(proof, !options.proof.empty(), "--proof set")
XORCLE_ENUM_OPTION(proof, proof_format, ProofFormat, ProofFormat::binary,
                   "format of the proof file")
XORCLE_ENUM_VALUE(proof_format, "binary", ProofFormat::binary)
XORCLE_ENUM_VALUE(proof_format, "text", ProofFormat::text)
XORCLE_OPTION(always, stats_file, Text, "", "", "",
              "output path for CSV statistics, suppresses CLI statistics")
XORCLE_GROUP(console, options.stats_file.empty(), "--stats-file unset")
XORCLE_GROUP(report_columns, options.stats_file.empty() && options.report_cadence != 0,
             "--stats-file unset and --report-cadence > 0")
XORCLE_OPTION(console, report_cadence, Size, 100, 0, SIZE_MAX,
              "conflicts between progress report lines (0 = off)")
XORCLE_OPTION(report_columns, report_type, Name, "default", "", "",
              "progress columns (default/search/deletion/propagation/all, comma-joined)")

#undef XORCLE_OPTION
#undef XORCLE_ENUM_OPTION
#undef XORCLE_ENUM_VALUE
#undef XORCLE_GROUP
