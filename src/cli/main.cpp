/// @file main.cpp
/// @brief The `xorcle` command line.
///
/// **Provisional.** Phase 3 replaces this with an X-macro
/// option table that generates the options struct, the parser, the bounds checks
/// and `--help` from one list. What is here already commits to the shape that table
/// produces -- `--name=value` options and the single positional `<input>`, with
/// the proof path moved to `--proof=` -- so Phase 3 is a
/// refactor of the parser rather than a redesign of the interface.

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "algebra/algebra.h"
#include "core/solver.h"
#include "heuristics/decisions/csids.h"
#include "heuristics/decisions/decision_procedure.h"
#include "heuristics/decisions/fixed_literals.h"
#include "heuristics/decisions/phase.h"
#include "heuristics/decisions/variable_vsids.h"
#include "heuristics/restarts/restart_policy.h"
#include "heuristics/selection/clause_selector.h"
#include "heuristics/selection/cyclic.h"
#include "heuristics/selection/leading_term_watchlist.h"
#include "io/dimacs.h"
#include "io/lrup_logger.h"
#include "util/random.h"
#include "util/stats.h"
#include "util/stats_sink.h"

#ifndef XORCLE_VERSION
#define XORCLE_VERSION "0.0.0"
#endif

namespace xorcle {

namespace {

/// @brief A malformed command line, or one whose values do not make sense.
///
/// User-facing input, so an exception caught in `main` rather than an assert.
class CliError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// @brief Everything the command line configures.
struct Options {
    Algebra algebra = Algebra::dense;
    Seed seed = default_seed;
    CompactionConfig compaction;
    DecisionProcedureName decisions = DecisionProcedureName::csids;
    CsidsConfig csids = csids_default_config;
    DecisionPhase phase = default_decision_phase;
    ClauseSelectorName selection = ClauseSelectorName::cyclic;
    RestartConfig restart;
    std::string proof_path;
    std::string stats_file;
    std::string input_path;

    /// @brief Name of the preset `--decisions=` chose, if it chose one.
    ///
    /// The presets fix the whole CSIDS triple, so an accompanying manual
    /// override is rejected rather than silently losing.
    std::string csids_preset;

    /// @brief Whether any `--csids-*` option was given.
    bool csids_overridden = false;
};

// ---------------------------------------------------------------------------
// Value parsing
// ---------------------------------------------------------------------------

/// @brief Parse an unsigned integer, rejecting signs and trailing junk.
std::uint64_t parse_u64(std::string_view name, const std::string& text) {
    if (text.empty() || text[0] == '-' || text[0] == '+') {
        throw CliError(std::string(name) + " expects a non-negative integer, got '" + text + "'");
    }
    try {
        std::size_t consumed = 0;
        const unsigned long long value = std::stoull(text, &consumed);
        if (consumed != text.size()) {
            throw CliError(std::string(name) + " expects an integer, got '" + text + "'");
        }
        return static_cast<std::uint64_t>(value);
    } catch (const std::logic_error&) {
        throw CliError(std::string(name) + " expects an integer, got '" + text + "'");
    }
}

/// @brief Parse a size, rejecting values below `minimum`.
std::size_t parse_size(std::string_view name, const std::string& text, std::size_t minimum) {
    const std::uint64_t value = parse_u64(name, text);
    if (value > std::numeric_limits<std::size_t>::max() ||
        static_cast<std::size_t>(value) < minimum) {
        throw CliError(std::string(name) + " must be at least " + std::to_string(minimum));
    }
    return static_cast<std::size_t>(value);
}

/// @brief Parse a finite double, rejecting trailing junk.
double parse_double(std::string_view name, const std::string& text) {
    try {
        std::size_t consumed = 0;
        const double value = std::stod(text, &consumed);
        if (consumed != text.size() || !std::isfinite(value)) {
            throw CliError(std::string(name) + " expects a finite number, got '" + text + "'");
        }
        return value;
    } catch (const std::logic_error&) {
        throw CliError(std::string(name) + " expects a finite number, got '" + text + "'");
    }
}

/// @brief Parse a boolean, accepting the 0.1 tree's `on`/`off` spellings too.
bool parse_bool(std::string_view name, const std::string& text) {
    if (text == "true" || text == "on" || text == "1" || text == "yes") {
        return true;
    }
    if (text == "false" || text == "off" || text == "0" || text == "no") {
        return false;
    }
    throw CliError(std::string(name) + " expects true or false, got '" + text + "'");
}

// ---------------------------------------------------------------------------
// Enum-valued options
//
// One name table per enum, serving both directions: parsing an option value and
// labelling it back in the statistics output. Phase 3 generates these alongside
// the option table; the shape is already what that
// generator produces.
// ---------------------------------------------------------------------------

/// @brief One accepted spelling of an enum value.
template <class T>
using NameTable = std::span<const std::pair<std::string_view, T>>;

constexpr std::pair<std::string_view, Algebra> algebra_names[]{{"dense", Algebra::dense},
                                                               {"sparse", Algebra::sparse}};

/// @note `berkmin` and `cmtf` are CSIDS presets rather than distinct procedures,
/// so they map to the same enumerator and `csids` is listed first: a reverse
/// lookup for the statistics label must find `csids`.
constexpr std::pair<std::string_view, DecisionProcedureName> decision_names[]{
    {"csids", DecisionProcedureName::csids},
    {"fixed_literals", DecisionProcedureName::fixed_literals},
    {"variable_vsids", DecisionProcedureName::variable_vsids},
    {"berkmin", DecisionProcedureName::csids},
    {"cmtf", DecisionProcedureName::csids}};

constexpr std::pair<std::string_view, DecisionPhase> phase_names[]{
    {"falsify", DecisionPhase::falsify},
    {"satisfy", DecisionPhase::satisfy},
    {"random", DecisionPhase::random}};

constexpr std::pair<std::string_view, ClauseSelectorName> selection_names[]{
    {"cyclic", ClauseSelectorName::cyclic},
    {"leading_term_watchlist", ClauseSelectorName::leading_term_watchlist}};

constexpr std::pair<std::string_view, RestartMethod> restart_names[]{
    {"none", RestartMethod::none},
    {"geometric", RestartMethod::geometric},
    {"luby", RestartMethod::luby}};

/// @brief Look a value up in a name table, or report every accepted spelling.
template <class T>
T parse_enum(std::string_view name, const std::string& text, NameTable<T> table) {
    std::string accepted;
    for (const auto& [spelling, value] : table) {
        if (text == spelling) {
            return value;
        }
        accepted += accepted.empty() ? "" : ", ";
        accepted += spelling;
    }
    throw CliError(std::string(name) + "='" + text + "' is not one of: " + accepted);
}

/// @brief The first spelling in `table` naming `value`; the statistics label.
template <class T>
std::string label_of(NameTable<T> table, T value) {
    for (const auto& [spelling, candidate] : table) {
        if (candidate == value) {
            return std::string(spelling);
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// The option table, as a parser
// ---------------------------------------------------------------------------

/// @brief Print the usage line and every option.
void print_help(std::ostream& out, const char* program_name) {
    out << "xorcle " << XORCLE_VERSION << "\n"
        << "Usage: " << program_name << " [options] <input>\n"
        << "\nOptions:\n"
        << "  --algebra=dense|sparse        Row storage backend (default: dense)\n"
        << "  --seed=N                      Base solver seed (default: " << default_seed << ")\n"
        << "  --compact=true|false          Variable compaction (default: true)\n"
        << "  --compact-every=N             Conflicts between compactions (default: 16)\n"
        << "  --decisions=NAME              fixed_literals, variable_vsids, csids,\n"
        << "                                berkmin, cmtf (default: csids)\n"
        << "  --csids-initial=X             A0: new asserting clause activity (default: 1)\n"
        << "  --csids-decay=X               alpha: decay per conflict, 0 < X < 1 (default: 0.98)\n"
        << "  --csids-bump=X                delta: activity bump (default: 1)\n"
        << "  --phase=falsify|satisfy|random  Decision phase (default: falsify)\n"
        << "  --selection=cyclic|leading_term_watchlist  Clause selector (default: cyclic)\n"
        << "  --restart=none|geometric|luby   EXPERIMENTAL restart schedule (default: none)\n"
        << "  --restart-geometric-factor=X  Geometric growth factor, X >= 1 (default: 1.5)\n"
        << "  --restart-initial=N           First geometric budget (default: 100)\n"
        << "  --restart-luby-unit=N         Budget for a Luby value of 1 (default: 100)\n"
        << "  --proof=PATH                  Write an LRUP(+) proof to PATH\n"
        << "  --stats-file=PATH             Append CSV statistics to PATH\n"
        << "  --help                        Show this message\n"
        << "\nNotes:\n"
        << "  --proof requires --compact=false, and does not turn compaction off for\n"
        << "  you: it refuses instead. Compaction changes the search rather than only\n"
        << "  its bookkeeping, so a run that silently disabled it would answer a\n"
        << "  different question than the one asked. Proof emission with compaction\n"
        << "  on is not implemented in 0.2; the paper's artifact supports it.\n"
        << "  --restart= is experimental and untuned.\n";
}

/// @brief Apply one `--name=value` option to `options`.
void apply_option(Options& options, const std::string& name, const std::string& value) {
    if (name == "--algebra") {
        options.algebra = parse_enum<Algebra>(name, value, algebra_names);
    } else if (name == "--seed") {
        options.seed = parse_u64(name, value);
    } else if (name == "--compact") {
        options.compaction.enabled = parse_bool(name, value);
    } else if (name == "--compact-every") {
        options.compaction.conflict_interval = parse_size(name, value, 1);
    } else if (name == "--decisions") {
        if (value == "berkmin" || value == "cmtf") {
            options.decisions = DecisionProcedureName::csids;
            options.csids = value == "berkmin" ? csids_berkmin_config : csids_cmtf_config;
            options.csids_preset = value;
        } else {
            options.csids_preset.clear();
            options.decisions = parse_enum<DecisionProcedureName>(name, value, decision_names);
        }
    } else if (name == "--csids-initial") {
        options.csids.initial_activity = parse_double(name, value);
        options.csids_overridden = true;
    } else if (name == "--csids-decay") {
        options.csids.decay = parse_double(name, value);
        options.csids_overridden = true;
    } else if (name == "--csids-bump") {
        options.csids.bump = parse_double(name, value);
        options.csids_overridden = true;
    } else if (name == "--phase") {
        options.phase = parse_enum<DecisionPhase>(name, value, phase_names);
    } else if (name == "--selection") {
        options.selection = parse_enum<ClauseSelectorName>(name, value, selection_names);
    } else if (name == "--restart") {
        options.restart.method = parse_enum<RestartMethod>(name, value, restart_names);
    } else if (name == "--restart-geometric-factor") {
        options.restart.geometric_multiplier = parse_double(name, value);
        if (options.restart.geometric_multiplier < 1.0) {
            throw CliError("--restart-geometric-factor must be at least 1");
        }
    } else if (name == "--restart-initial") {
        options.restart.geometric_initial_conflicts = parse_size(name, value, 1);
    } else if (name == "--restart-luby-unit") {
        options.restart.luby_unit_conflicts = parse_size(name, value, 1);
    } else if (name == "--proof") {
        options.proof_path = value;
    } else if (name == "--stats-file") {
        options.stats_file = value;
    } else {
        throw CliError("unknown option '" + name + "'");
    }
}

/// @brief Parse the whole command line.
/// @return The options, or `std::nullopt` if `--help` was given and printed.
std::optional<Options> parse_command_line(int argc, char* argv[]) {
    Options options;
    std::vector<std::string> positionals;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            print_help(std::cout, argv[0]);
            return std::nullopt;
        }
        if (argument.rfind("--", 0) != 0) {
            positionals.push_back(argument);
            continue;
        }
        const std::size_t equals = argument.find('=');
        if (equals == std::string::npos) {
            throw CliError("option '" + argument + "' needs a value, as --name=value");
        }
        apply_option(options, argument.substr(0, equals), argument.substr(equals + 1));
    }

    if (positionals.size() != 1) {
        throw CliError("expected exactly one input file; usage: " + std::string(argv[0]) +
                       " [options] <input>");
    }
    options.input_path = positionals.front();

    if (!options.csids_preset.empty() && options.csids_overridden) {
        throw CliError("--decisions=" + options.csids_preset +
                       " is a preset and does not accept --csids-* overrides");
    }
    if (options.decisions == DecisionProcedureName::csids) {
        if (options.csids.initial_activity < 0.0) {
            throw CliError("--csids-initial must be at least 0");
        }
        if (options.csids.bump < 0.0) {
            throw CliError("--csids-bump must be at least 0");
        }
        if (options.csids.decay <= 0.0 || options.csids.decay >= 1.0) {
            throw CliError("--csids-decay must satisfy 0 < decay < 1");
        }
    }

    // Refuse rather than silently flipping
    // --compact. The message names the fix.
    require_compaction_disabled_for_proof(!options.proof_path.empty(),
                                          options.compaction.enabled);

    const std::filesystem::path input(options.input_path);
    if (!std::filesystem::exists(input) || !std::filesystem::is_regular_file(input)) {
        throw CliError("input is not a readable file: " + options.input_path);
    }
    return options;
}

// ---------------------------------------------------------------------------
// Building the heuristics
//
// The enum-to-object mapping has to live somewhere; Phase 3 moves it beside the
// option table it belongs to. The 0.1 tree kept it in `heuristics/Factory.cpp`.
// ---------------------------------------------------------------------------

/// @brief Build the decision procedure the options name.
std::unique_ptr<DecisionProcedure> make_decision_procedure(const Options& options,
                                                           std::size_t num_vars) {
    std::unique_ptr<DecisionProcedure> procedure;
    switch (options.decisions) {
        case DecisionProcedureName::fixed_literals:
            procedure = std::make_unique<FixedLiterals>(options.algebra, num_vars, options.seed);
            break;
        case DecisionProcedureName::variable_vsids:
            procedure = std::make_unique<VariableVsids>(options.algebra, num_vars, options.seed);
            break;
        case DecisionProcedureName::csids:
            procedure = std::make_unique<Csids>(options.algebra, num_vars, options.seed,
                                                options.csids);
            break;
    }
    procedure->set_decision_phase(options.phase);
    return procedure;
}

/// @brief Build the clause selector the options name.
std::unique_ptr<ClauseSelector> make_clause_selector(const Options& options,
                                                     std::size_t num_vars) {
    switch (options.selection) {
        case ClauseSelectorName::cyclic:
            return std::make_unique<Cyclic>(num_vars);
        case ClauseSelectorName::leading_term_watchlist:
            return std::make_unique<LeadingTermWatchlist>(num_vars);
    }
    return std::make_unique<Cyclic>(num_vars);
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

/// @brief The current UTC time as `%Y-%m-%dT%H:%M:%SZ`.
std::string utc_timestamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::ostringstream text;
    text << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return text.str();
}

/// @brief A run identifier: the timestamp plus milliseconds since the epoch.
std::string run_id_from_timestamp(const std::string& timestamp_utc) {
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    return timestamp_utc + "-" + std::to_string(millis);
}

/// @brief Solve one instance and collect its statistics.
RunStats run_once(const Options& options) {
    ParsedDimacs parsed = parse_dimacs_file(options.input_path, options.algebra);

    std::unique_ptr<LrupLogger> proof_logger;
    if (!options.proof_path.empty()) {
        proof_logger = std::make_unique<LrupLogger>(options.proof_path, parsed.num_vars);
    }

    Solver solver(options.algebra, parsed.num_vars,
                  make_clause_selector(options, parsed.num_vars),
                  make_decision_procedure(options, parsed.num_vars),
                  make_restart_policy(options.restart), proof_logger.get(), options.compaction);
    for (Clause& clause : parsed.clauses) {
        solver.add_starting_clause(std::move(clause));
    }

    const std::size_t start_clauses = solver.num_clauses();
    const auto start = std::chrono::high_resolution_clock::now();
    Solution solution = solver.solve();
    const std::chrono::duration<double> elapsed = std::chrono::high_resolution_clock::now() - start;

    if (proof_logger) {
        // A proof of a satisfiable instance proves nothing, so it is discarded.
        proof_logger->finalize(solution.result == SolveResult::unsat);
    }

    RunStats stats;
    stats.file = options.input_path;
    stats.decision_procedure = label_of<DecisionProcedureName>(decision_names, options.decisions);
    stats.decision_phase = label_of<DecisionPhase>(phase_names, options.phase);
    stats.clause_selector = label_of<ClauseSelectorName>(selection_names, options.selection);
    stats.seed = options.seed;
    if (options.decisions == DecisionProcedureName::csids) {
        stats.csids_initial_activity = options.csids.initial_activity;
        stats.csids_decay = options.csids.decay;
        stats.csids_bump = options.csids.bump;
    }
    stats.result = solution.result;
    stats.sat_assignment = std::move(solution.assignment);
    stats.time_s = elapsed.count();
    stats.num_vars = solver.num_vars();
    stats.start_clauses = start_clauses;
    stats.final_clauses = solver.num_clauses();

    const SolverCounters& counters = solver.counters();
    stats.num_conflicts = counters.num_conflicts;
    stats.num_propagations = counters.num_propagations;
    stats.num_attempt_props = counters.num_attempt_props;
    stats.num_watch_updates = counters.num_watch_updates;
    stats.num_restarts = counters.num_restarts;

    const DecisionProcedure::DecisionStats decisions = solver.decision_stats();
    stats.decisions = decisions.decisions;
    stats.fallback_decisions = decisions.fallback_decisions;
    return stats;
}

} // namespace

} // namespace xorcle

/// @brief Entry point: parse, solve, report.
/// @return 0 on success, SAT and UNSAT alike; 1 on any reported error.
int main(int argc, char* argv[]) {
    using namespace xorcle;
    try {
        const std::optional<Options> options = parse_command_line(argc, argv);
        if (!options.has_value()) {
            return 0;
        }

        const std::string timestamp_utc = utc_timestamp();
        RunStats stats = run_once(*options);
        stats.timestamp_utc = timestamp_utc;
        stats.run_id = run_id_from_timestamp(timestamp_utc);

        std::unique_ptr<StatsSink> sink;
        if (options->stats_file.empty()) {
            sink = std::make_unique<ConsoleStatsSink>();
        } else {
            sink = std::make_unique<CsvStatsSink>(options->stats_file);
        }
        sink->log(stats);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << std::endl;
        return 1;
    }
}
