/// @file main.cpp
/// @brief The `xorcle` executable: parse a command line, solve one instance,
/// report.
///
/// Thin by design. Every option lives in `cli/options.inc` and everything that
/// reads it lives in `cli/options.h`, so what is
/// left here is the enum-to-object mapping the 0.1 tree kept in
/// `heuristics/Factory.cpp`, the run itself, and the error boundary
/// around both.

#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "cli/options.h"
#include "core/solver.h"
#include "heuristics/decisions/csids.h"
#include "heuristics/decisions/decision_procedure.h"
#include "heuristics/decisions/fixed_literals.h"
#include "heuristics/decisions/variable_vsids.h"
#include "heuristics/restarts/restart_policy.h"
#include "heuristics/selection/clause_selector.h"
#include "heuristics/selection/cyclic.h"
#include "heuristics/selection/leading_term_watchlist.h"
#include "io/dimacs.h"
#include "io/lrup_logger.h"
#include "util/stats.h"
#include "util/stats_sink.h"

#ifndef XORCLE_VERSION
#define XORCLE_VERSION "0.0.0"
#endif

namespace xorcle {

namespace {

// ---------------------------------------------------------------------------
// Building the heuristics
//
// The enum-to-object mapping has to live somewhere, and it belongs beside the
// option table that names the enumerators.
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
                                                csids_config(options));
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
    if (!options.proof.empty()) {
        proof_logger = std::make_unique<LrupLogger>(options.proof, parsed.num_vars);
    }

    Solver solver(options.algebra, parsed.num_vars,
                  make_clause_selector(options, parsed.num_vars),
                  make_decision_procedure(options, parsed.num_vars),
                  make_restart_policy(restart_config(options)), proof_logger.get(),
                  compaction_config(options));
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
    // The labels are the table's own spellings, so `--decisions=berkmin` reports
    // `csids`: the first spelling naming an enumerator is its label.
    stats.decision_procedure = enum_option_label("decisions", static_cast<int>(options.decisions));
    stats.decision_phase = enum_option_label("phase", static_cast<int>(options.phase));
    stats.clause_selector = enum_option_label("selection", static_cast<int>(options.selection));
    stats.seed = options.seed;
    if (options.decisions == DecisionProcedureName::csids) {
        stats.csids_initial_activity = options.csids_initial;
        stats.csids_decay = options.csids_decay;
        stats.csids_bump = options.csids_bump;
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

/// @brief Check the positional input names a file we can read.
/// @throws CliError if it does not.
void require_readable_input(const std::string& input_path) {
    const std::filesystem::path input(input_path);
    if (!std::filesystem::exists(input) || !std::filesystem::is_regular_file(input)) {
        throw CliError("input is not a readable file: " + input_path);
    }
}

} // namespace

} // namespace xorcle

/// @brief Entry point: parse, solve, report.
/// @return 0 on success, SAT and UNSAT alike; 1 on any reported error.
int main(int argc, char* argv[]) {
    using namespace xorcle;
    try {
        const std::vector<std::string> arguments(argv + (argc > 0 ? 1 : 0), argv + argc);
        const std::string program_name = argc > 0 ? argv[0] : "xorcle";
        const std::optional<Options> options =
            parse_options(arguments, std::cout, program_name, XORCLE_VERSION);
        if (!options.has_value()) {
            return 0;
        }
        require_readable_input(options->input_path);

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
