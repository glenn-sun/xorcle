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
#include "heuristics/decisions/clause_activity.h"
#include "heuristics/decisions/csids.h"
#include "heuristics/decisions/decision_procedure.h"
#include "heuristics/decisions/fixed_literals.h"
#include "heuristics/deletion/deletion.h"
#include "heuristics/restarts/restart_policy.h"
#include "heuristics/selection/clause_selector.h"
#include "heuristics/selection/cyclic.h"
#include "heuristics/selection/exact_watchlist.h"
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

// --- Building the heuristics -----------------------------------------------

std::unique_ptr<DecisionProcedure> make_decision_procedure(const Options& options,
                                                           std::size_t num_vars) {
    std::unique_ptr<DecisionProcedure> procedure;
    switch (options.decisions) {
        case DecisionProcedureName::fixed_literals:
            procedure =
                std::make_unique<FixedLiterals>(options.equation_repr, num_vars, options.seed);
            break;
        case DecisionProcedureName::csids:
            procedure = std::make_unique<Csids>(options.equation_repr, num_vars, options.seed,
                                                csids_config(options));
            break;
    }
    procedure->set_decision_phase(options.phase);
    return procedure;
}

std::unique_ptr<ClauseSelector> make_clause_selector(const Options& options,
                                                     std::size_t num_vars) {
    switch (options.selection) {
        case ClauseSelectorName::cyclic:
            return std::make_unique<Cyclic>(num_vars);
        case ClauseSelectorName::exact_watchlist:
            return std::make_unique<ExactWatchlist>(num_vars);
    }
    assert(false && "make_clause_selector: unknown clause selector name");
    return std::make_unique<Cyclic>(num_vars);
}

// --- Proofs ----------------------------------------------------------------

/// @brief Where the raw proof goes: the requested path, or a temporary next to
/// the final proof.
std::filesystem::path raw_proof_path(const Options& options) {
    return options.proof_raw.empty() ? std::filesystem::path(options.proof + ".raw")
                                     : std::filesystem::path(options.proof_raw);
}

/// @brief The input clauses' rows, indexed by clause id, as post-processing
/// wants them: already in the original numbering, so no lifting is needed.
std::vector<std::vector<RawRow>> input_clause_rows(const std::vector<Clause>& clauses) {
    std::vector<std::vector<RawRow>> rows(clauses.size());
    for (std::size_t id = 0; id < clauses.size(); ++id) {
        for (std::size_t index = 0; index < clauses[id].rows(); ++index) {
            rows[id].push_back(clauses[id].row(index).bits());
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
/// @throws std::runtime_error On a raw proof no proof can be made of.
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
        // Only on success: a raw proof no proof could be made of is the one
        // thing a post-processing failure can be debugged from.
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
        input_rows = input_clause_rows(parsed.clauses);
    }

    // Progress goes to stdout beside the DIMACS output, so a run writing its
    // statistics to a file reports nothing here: the file is the output then.
    std::unique_ptr<ProgressReporter> progress_reporter;
    if (options.report_cadence != 0 && options.stats_file.empty()) {
        const std::optional<unsigned> groups = parse_progress_groups(options.report);
        assert(groups.has_value() && "validate_options() should have checked --report");
        progress_reporter = std::make_unique<ConsoleProgressReporter>(std::cout, *groups);
    }

    std::unique_ptr<ClauseSelector> selector = make_clause_selector(options, parsed.num_vars);
    std::unique_ptr<DecisionProcedure> decision_procedure =
        make_decision_procedure(options, parsed.num_vars);

    DeletionConfig deletion = deletion_config(options);
    selector->set_tier_config(deletion.tiers);
    if (deletion.enabled && deletion.key == DeletionKey::activity) {
        deletion.activity = dynamic_cast<const ClauseActivitySource*>(decision_procedure.get());
        assert(deletion.activity != nullptr &&
               "validate_options() should have required a scoring decision procedure");
    }

    Solver solver(options.equation_repr, parsed.num_vars, std::move(selector),
                  std::move(decision_procedure),
                  make_restart_policy(restart_config(options)), proof_writer.get(),
                  compaction_config(options),
                  ProgressConfig{.reporter = progress_reporter.get(),
                                 .cadence = options.report_cadence},
                  options.minimize, deletion, options.bump_responsible_clauses, options.subsume,
                  propagation_config(options),
                  implication_graph_config(options));
    for (Clause& clause : parsed.clauses) {
        solver.add_starting_clause(std::move(clause));
    }

    const std::size_t start_clauses = solver.num_clauses();
    const auto start = std::chrono::steady_clock::now();
    Solution solution = solver.solve();
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

    stats.num_vars = solver.num_vars();
    stats.active_vars = solver.active_num_vars();
    stats.start_clauses = start_clauses;

    const SolverCounters& counters = solver.counters();
    stats.num_conflicts = counters.num_conflicts;
    stats.num_propagations = counters.num_propagations;
    stats.num_attempt_props = counters.num_attempt_props;
    stats.num_watch_updates = counters.num_watch_updates;
    stats.num_compactions = counters.num_compactions;
    stats.num_deferred_runs = counters.num_deferred_runs;
    stats.num_out_of_order_learns = counters.num_out_of_order_learns;
    stats.num_undone_decisions = counters.num_undone_decisions;
    stats.num_decision_propagations = counters.num_decision_propagations;
    stats.num_decision_conflicts = counters.num_decision_conflicts;
    stats.mean_budget_fraction =
        counters.num_budgeted_runs == 0
            ? 0.0
            : counters.sum_budget_fraction / static_cast<double>(counters.num_budgeted_runs);
    stats.final_budget_fraction = counters.final_budget_fraction;

    const DecisionProcedure::DecisionStats decisions = solver.decision_stats();
    stats.decisions = decisions.decisions;
    stats.fallback_decisions = decisions.fallback_decisions;
    stats.intersection_decisions = decisions.intersections.decisions;
    stats.intersected_clauses = decisions.intersections.intersected_clauses;
    stats.intersection_dimension_before = decisions.intersections.dimension_before;
    stats.intersection_dimension_after = decisions.intersections.dimension_after;
    stats.intersection_passed_satisfied_clauses =
        decisions.intersections.passed_satisfied_clauses;
    stats.intersection_propagating_clauses = decisions.intersections.propagating_clauses;
    stats.intersection_time_s = decisions.intersections.time_s;

    const RestartStats restarts = solver.restart_stats();
    stats.num_restarts = counters.num_restarts;
    stats.num_reused_levels = counters.num_reused_levels;
    stats.num_canceled_restarts = restarts.canceled;
    stats.fast_lbd = restarts.fast_lbd;
    stats.slow_lbd = restarts.slow_lbd;

    // Ids are never reused, so what was allocated and what is still there part
    // company as soon as deletion runs.
    stats.clauses_allocated = solver.num_clauses();
    stats.clauses_in_database = solver.num_active_clauses();
    stats.learned_clauses = solver.num_clauses() - start_clauses;
    stats.average_learned_lbd =
        counters.num_asserting_clauses == 0
            ? 0.0
            : static_cast<double>(counters.sum_learned_lbd) /
                  static_cast<double>(counters.num_asserting_clauses);

    stats.learned_equation_bits = counters.learned_equation_bits;
    stats.learned_width_1 = counters.num_learned_width_1;
    stats.learned_width_2 = counters.num_learned_width_2;
    stats.subsume_narrowed_to_width_2 = counters.num_subsume_narrowed_to_width_2;
    stats.compaction_narrowed_to_width_2 = counters.num_compaction_narrowed_to_width_2;

    stats.num_minimized_clauses = counters.num_minimized_clauses;
    stats.minimize_width_before = counters.minimize_width_before;
    stats.minimize_width_after = counters.minimize_width_after;

    stats.num_subsumed_clauses = counters.num_subsumed_clauses;
    stats.num_subsumed_conflicting_clauses = counters.num_subsumed_conflicting_clauses;
    stats.num_subsume_replacements = counters.num_subsume_replacements;

    stats.num_deletion_passes = counters.num_deletion_passes;
    stats.num_deleted_clauses = counters.num_deleted_clauses;
    stats.num_demoted_clauses = counters.num_demoted_clauses;
    stats.num_promoted_clauses = counters.num_promoted_clauses;

    const ImplicationGraphCounters& graph = solver.implication_graph_counters();
    stats.implication_graph_runs = graph.num_runs;
    stats.implication_graph_edges_visited = graph.num_edges_visited;
    stats.implication_graph_pairs_eligible = graph.num_pairs_eligible;
    stats.implication_graph_pairs_examined = graph.num_pairs_examined;
    stats.implication_graph_refuted_sides = graph.num_refuted_sides;
    stats.implication_graph_runs_at_the_pair_cap = graph.num_runs_at_the_pair_cap;
    stats.implication_graph_truncated_sides = graph.num_truncated_sides;
    stats.implication_graph_units_fed = graph.num_units_fed;
    stats.implication_graph_runs_after_a_conflict = graph.num_runs_after_a_conflict;
    stats.implication_graph_runs_due_to_clauses = graph.num_runs_due_to_clauses;
    stats.implication_graph_runs_due_to_conflicts = graph.num_runs_due_to_conflicts;
    stats.implication_graph_runs_due_to_a_feed = graph.num_runs_due_to_a_feed;
    stats.implication_graph_follow_ups_suppressed = graph.num_follow_ups_suppressed;
    stats.implication_graph_conflicts_waited = graph.num_conflicts_waited;
    stats.implication_graph_max_conflicts_waited = graph.max_conflicts_waited;
    stats.implication_graph_width_2_clauses = solver.implication_graph().num_width_2_clauses();
    stats.implication_graph_vertex_pairs = solver.implication_graph().num_vertex_pairs();
    stats.implication_graph_forced_compactions = graph.num_forced_compactions;
    stats.implication_graph_span_intersection_time_s = graph.span_intersection_time_s;
    stats.implication_graph_time_s = graph.time_s;

    solver.selector().add_to_stats(stats);

    stats.parse_time_s = parse_elapsed.count();
    stats.solve_time_s = elapsed.count();
    stats.decision_time_s = counters.decision_time_s;
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
        std::cerr << "Error: " << error.what() << std::endl;
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << std::endl;
        return 1;
    }
}
