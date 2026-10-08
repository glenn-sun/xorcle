/// @file main.cpp
/// @brief The `xorcle` executable: parse a command line, solve one instance,
/// report.

#include <cassert>
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
#include <system_error>
#include <utility>
#include <vector>

#include "cli/options.h"
#include "core/solver.h"
#include "io/dimacs.h"
#include "io/proof_postprocess.h"
#include "io/proof_raw.h"
#include "io/proof_raw_file.h"
#include "util/progress_reporter.h"
#include "util/stats.h"
#include "util/stats_writer.h"

#ifndef XORCLE_VERSION
#define XORCLE_VERSION "0.0.0"
#endif

namespace xorcle {

namespace {

// --- Proofs ----------------------------------------------------------------

/// @brief Where the raw proof goes: the requested path, or a temporary next to
/// the final proof.
std::filesystem::path raw_proof_path(const Options& options) {
    return options.proof_raw.empty() ? std::filesystem::path(options.proof + ".raw")
                                     : std::filesystem::path(options.proof_raw);
}

/// @brief The input clauses' rows in the original numbering, indexed by clause
/// id, as post-processing wants them.
std::vector<std::vector<RawRow>> input_clause_rows(const ParsedDimacs& parsed) {
    std::vector<std::vector<RawRow>> rows(parsed.clauses.size());
    for (std::size_t id = 0; id < parsed.clauses.size(); ++id) {
        for (const Equation& row : parsed.clauses[id]) {
            rows[id].push_back(lift_row(row, parsed.used_vars, parsed.num_vars));
        }
    }
    return rows;
}

/// @brief The post-processing `run_once` leaves for after the verdict and the
/// stats have been reported; `raw_path` is empty when there is none to do.
struct PendingProof {
    std::filesystem::path raw_path;
    std::vector<std::vector<RawRow>> input_rows;
    bool keep_raw = false; ///< `--proof-raw` asked for the raw proof itself.
};

/// @brief Turn the raw proof into the final LRUP(⊕) proof, and report on it.
/// @return Wall-clock seconds spent post-processing and writing the proof.
/// @throws RawProofError If the raw proof cannot be read or the proof cannot be written.
double write_lrup_proof(const Options& options, const PendingProof& pending) {
    const auto start = std::chrono::steady_clock::now();
    PostprocessSummary summary;
    {
        RawProofFileSource source(pending.raw_path);
        AtomicOutputFile proof(options.proof, std::ios::out);
        summary = postprocess_raw_proof(source, source.num_vars(), source.num_input_clauses(),
                                        options.equation_repr, proof.out(), pending.input_rows);
        proof.finalize(true);
    }
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;

    if (!pending.keep_raw) {
        // Only on success: a failed post-processing leaves the raw proof behind.
        std::error_code ec;
        std::filesystem::remove(pending.raw_path, ec);
    }

    std::cout << "c Proof: " << summary.kept_learned << " of " << summary.raw_learned
              << " learned lines kept, " << summary.rewrite_lines << " rewrite lines citing "
              << summary.rewrite_unit_ids << " unit ids, " << summary.deletion_lines
              << " deletion lines, post-processing " << std::fixed << std::setprecision(3)
              << elapsed.count() << " s\n";
    return elapsed.count();
}

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

/// @brief Solve one instance; on UNSAT with `--proof`, leave the raw proof for
/// `pending` so the caller can report before post-processing it.
RunStats run_once(const Options& options, PendingProof& pending) {
    const auto parse_start = std::chrono::steady_clock::now();
    ParsedDimacs parsed = parse_dimacs_file(options.input_path, options.equation_repr);
    const std::chrono::duration<double> parse_elapsed =
        std::chrono::steady_clock::now() - parse_start;

    // The solver streams a raw proof; post-processing turns it into the proof.
    const bool wants_proof = !options.proof.empty();
    const bool wants_raw = !options.proof_raw.empty();
    std::unique_ptr<RawProofWriter> proof_writer;
    std::filesystem::path raw_path;
    std::vector<std::vector<RawRow>> input_rows;
    if (wants_proof || wants_raw) {
        raw_path = raw_proof_path(options);
        proof_writer =
            std::make_unique<RawProofWriter>(raw_path, parsed.num_vars, parsed.clauses.size());
    }
    if (wants_proof) {
        // The solver takes the clauses over, so their rows are kept beforehand.
        input_rows = input_clause_rows(parsed);
    }

    // Progress goes to stdout beside the DIMACS output, so a run writing its
    // statistics to a file reports nothing here: the file is the output then.
    std::unique_ptr<ProgressReporter> progress_reporter;
    if (options.report_cadence != 0 && options.stats_file.empty()) {
        const std::optional<unsigned> groups = parse_progress_groups(options.report);
        assert(groups.has_value() && "validate_options() should have checked --report");
        progress_reporter = std::make_unique<ConsoleProgressReporter>(std::cout, *groups);
    }

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

    const bool unsat = solution.result == SolveResult::unsat;
    if (proof_writer) {
        proof_writer->finalize(wants_raw || (wants_proof && unsat));
    }
    if (wants_proof && unsat) {
        pending = PendingProof{raw_path, std::move(input_rows), wants_raw};
    }

    RunStats stats;
    stats.file = options.input_path;
    stats.config = option_settings(options);
    stats.result = solution.result;
    stats.sat_assignment = std::move(solution.assignment);

    solver->add_to_stats(stats);
    stats.start_clauses = start_clauses;
    stats.learned_clauses = solver->num_clauses() - start_clauses;

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
        PendingProof pending;
        RunStats stats = run_once(*options, pending);
        stats.timestamp_utc = timestamp_utc;
        stats.run_id = run_id_from_timestamp(timestamp_utc);

        const bool to_console = options->stats_file.empty();
        std::unique_ptr<StatsWriter> writer;
        if (to_console) {
            writer = std::make_unique<ConsoleStatsWriter>();
        } else {
            writer = std::make_unique<CsvStatsWriter>(options->stats_file);
        }

        // The console gets the verdict and the stats before post-processing,
        // which can take a while; the CSV waits for its post-processing column,
        // so its verdict goes out first on its own.
        if (to_console) {
            writer->log(stats);
        } else {
            write_verdict(std::cout, stats);
        }
        if (!pending.raw_path.empty()) {
            stats.proof_postprocess_time_s = write_lrup_proof(*options, pending);
        }
        if (!to_console) {
            writer->log(stats);
        }
        return stats.result == SolveResult::sat ? 10 : 20;
    } catch (const CliError& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
