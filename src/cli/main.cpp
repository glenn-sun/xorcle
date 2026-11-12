/// @file main.cpp
/// @brief The `xorcle` executable: parse a command line, solve one instance,
/// report.

#include <cassert>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "algebra/equation.h"
#include "algebra/row_echelon_matrix.h"
#include "cli/options.h"
#include "core/solver.h"
#include "io/dimacs.h"
#include "io/proof_writer.h"
#include "util/index_set.h"
#include "util/progress_reporter.h"
#include "util/stats.h"
#include "util/stats_writer.h"

#ifndef XORCLE_VERSION
#define XORCLE_VERSION "0.0.0"
#endif

namespace xorcle {

namespace {

// --- Running ---------------------------------------------------------------

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

#ifndef NDEBUG
/// @brief Whether @p assignment, over the original variables, satisfies every clause,
/// each over @p used_vars as the parser leaves it: in each, some row whose set bits, the
/// constant bit counted as set, have odd parity under it.
bool debug_satisfies(const std::vector<Clause>& clauses, const std::vector<std::size_t>& used_vars,
                     const IndexSet& assignment) {
    for (const Clause& clause : clauses) {
        bool holds = false;
        for (const Equation& row : clause) {
            bool parity = false;
            for (const std::size_t bit : row.bits()) {
                parity ^= bit == used_vars.size() || assignment.test(used_vars[bit]);
            }
            if (parity) {
                holds = true;
                break;
            }
        }
        if (!holds) {
            return false;
        }
    }
    return true;
}
#endif

/// @brief Solve one instance.
/// @throws std::runtime_error If the `--proof` file cannot be opened or written.
RunStats run_once(const Options& options) {
    const auto parse_start = std::chrono::steady_clock::now();
    ParsedDimacs parsed = parse_dimacs_file(options.input_path, options.equation_repr);
    const std::chrono::duration<double> parse_elapsed =
        std::chrono::steady_clock::now() - parse_start;

    std::ofstream proof_file;
    std::unique_ptr<ProofWriter> proof_writer;
    if (!options.proof.empty()) {
        proof_file.open(options.proof, std::ios::binary);
        if (!proof_file.is_open()) {
            throw std::runtime_error("unable to open the proof file: " + options.proof);
        }
        proof_writer =
            std::make_unique<ProofWriter>(proof_file, parsed.num_vars, options.proof_format);
    }

    // Progress goes to stdout beside the DIMACS output, so a run writing its
    // statistics to a file reports nothing here: the file is the output then.
    std::unique_ptr<ConsoleProgressReporter> progress_reporter;
    if (options.report_cadence != 0 && options.stats_file.empty()) {
        const std::optional<unsigned> groups = parse_progress_groups(options.report_type);
        assert(groups.has_value() && "validate_options() should have checked --report-type");
        progress_reporter = std::make_unique<ConsoleProgressReporter>(std::cout, *groups);
    }

#ifndef NDEBUG
    const std::vector<Clause> starting_clauses = parsed.clauses;
    const std::vector<std::size_t> starting_used_vars = parsed.used_vars;
#endif
    const std::unique_ptr<Solver> solver =
        make_solver(options, parsed.num_vars, std::move(parsed.used_vars), proof_writer.get(),
                    progress_reporter.get());
    for (Clause& clause : parsed.clauses) {
        solver->add_starting_clause(std::move(clause));
    }

    const std::size_t start_clauses = solver->num_clauses();
    const auto start = std::chrono::steady_clock::now();
    Solution solution = solver->solve();
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    assert((solution.result != SolveResult::sat ||
            debug_satisfies(starting_clauses, starting_used_vars, *solution.assignment)) &&
           "the model does not satisfy the instance");

    if (proof_writer) {
        proof_writer->flush();
        proof_file.close();
        if (proof_file.fail()) {
            throw std::runtime_error("failed to write the proof file: " + options.proof);
        }
    }

    RunStats stats = solver->stats();
    stats.file = options.input_path;
    stats.config = option_settings(options);
    stats.result = solution.result;
    stats.sat_assignment = std::move(solution.assignment);
    stats.start_clauses = start_clauses;

    stats.parse_time_s = parse_elapsed.count();
    stats.solve_time_s = elapsed.count();
    return stats;
}

/// @throws CliError if the input does not name a readable regular file.
void require_readable_input(const std::string& input_path) {
    const std::filesystem::path input(input_path);
    if (!std::filesystem::exists(input) || !std::filesystem::is_regular_file(input)) {
        throw CliError("input is not a readable file: " + input_path);
    }
}

} // namespace

} // namespace xorcle

/// @brief Entry point: parse, solve, report.
/// @return 10 for SAT and 20 for UNSAT, as the SAT competition spells them; 0
/// when there is no instance to solve, such as `--help`; 2 on a command line
/// this build will not accept, and 1 on any other reported error. A harness
/// sweeping options tells a rejected combination from a failed run by that
/// difference.
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

        const bool to_console = options->stats_file.empty();
        std::unique_ptr<StatsWriter> writer;
        if (to_console) {
            writer = std::make_unique<ConsoleStatsWriter>();
        } else {
            writer = std::make_unique<CsvStatsWriter>(options->stats_file);
        }

        // The console's stats carry the verdict; the CSV's go to the file.
        if (!to_console) {
            write_verdict(std::cout, stats);
        }
        writer->log(stats);
        return stats.result == SolveResult::sat ? 10 : 20;
    } catch (const CliError& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
