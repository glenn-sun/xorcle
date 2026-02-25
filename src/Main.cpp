#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <getopt.h>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "search/Solver.h"
#include "heuristics/DecisionProcedure.h"
#include "heuristics/ClauseSelector.h"
#include "io/ProofLogger.h"
#include "util/Stats.h"
#include "util/StatsLogger.h"

namespace fs = std::filesystem;

namespace {

std::optional<double> parse_double_token(const char* token) {
    if (token == nullptr) {
        return std::nullopt;
    }
    try {
        std::size_t pos = 0;
        double value = std::stod(token, &pos);
        if (pos != std::string(token).size()) {
            return std::nullopt;
        }
        return value;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<std::size_t> parse_size_t_token(const char* token) {
    if (token == nullptr) {
        return std::nullopt;
    }
    std::string s(token);
    if (s.empty() || s[0] == '-') {
        return std::nullopt;
    }
    try {
        std::size_t pos = 0;
        unsigned long long value = std::stoull(s, &pos);
        if (pos != s.size()) {
            return std::nullopt;
        }
        if (value > std::numeric_limits<std::size_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace

/// @brief Output mode for statistics reporting.
enum class StatsMode {
    console, ///< Print stats to stdout.
    csv      ///< Append stats to a CSV file.
};

/// @brief Run the solver once and collect run statistics.
/// @param filename DIMACS file path.
/// @param dpn Decision procedure to use.
/// @param csn Clause selector to use.
/// @param stop_flag External stop signal to interrupt solving.
/// @param proof_path Optional proof output path.
stats::RunStats run_once(const std::string& filename,
                         heuristics::DecisionProcedureName dpn,
                         heuristics::ClauseSelectorName csn,
                         heuristics::DecisionPhase phase,
                         const search::RestartConfig& restart_config,
                         std::atomic<bool>& stop_flag,
                         const std::string& proof_path = {}) {
    std::shared_ptr<io::ProofLogger> proof_logger;
    if (!proof_path.empty()) {
        proof_logger = std::make_shared<io::ProofLogger>(proof_path);
    }
    search::Solver solver = search::Solver::from_DIMACS(filename, dpn, csn, phase, proof_logger, restart_config);
    std::size_t num_vars = solver.get_num_vars();
    std::size_t start_num_clauses = solver.get_num_clauses();

    auto start = std::chrono::high_resolution_clock::now();
    auto result = solver.solve(stop_flag);
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;
    if (proof_logger) {
        proof_logger->finalize(search::ResultCase::is_unsat(result));
    }

    stats::RunStats out;
    out.num_vars = num_vars;
    out.start_clauses = start_num_clauses;
    out.final_clauses = solver.get_num_clauses();
    out.time_s = duration.count();
    out.num_conflicts = solver.num_conflicts;
    out.num_propagations = solver.num_propagations;
    out.num_attempt_props = solver.num_attempt_props;
    out.num_watch_updates = solver.num_watch_updates;
    out.num_restarts = solver.num_restarts;
    auto dec_stats = solver.decision_stats();
    out.decisions = dec_stats.decisions;
    out.fallback_decisions = dec_stats.fallback_decisions;

    if (search::ResultCase::is_timeout(result)) {
        out.result = stats::SolveResult::Timeout;
    } else if (search::ResultCase::is_unsat(result)) {
        out.result = stats::SolveResult::Unsat;
    } else if (search::ResultCase::is_sat(result)) {
        out.result = stats::SolveResult::Sat;
        std::ostringstream assign;
        assign << search::ResultCase::unwrap_sat(result);
        out.sat_assignment = assign.str();
    }

    return out;
}

/// @brief Run the solver once with a timeout and collect statistics.
/// @param filename DIMACS file path.
/// @param dpn Decision procedure to use.
/// @param csn Clause selector to use.
/// @param timeout_s Timeout in seconds.
/// @param proof_path Optional proof output path.
/// @return RunStats on success; std::nullopt on error.
std::optional<stats::RunStats> run_once_with_timeout(const std::string& filename,
                                                     heuristics::DecisionProcedureName dpn,
                                                     heuristics::ClauseSelectorName csn,
                                                     heuristics::DecisionPhase phase,
                                                     const search::RestartConfig& restart_config,
                                                     std::size_t timeout_s,
                                                     const std::string& proof_path = {}) {
    std::optional<stats::RunStats> result;
    std::exception_ptr err;
    std::atomic<bool> stop_flag(false);
    std::mutex mtx;
    std::condition_variable cv;
    bool completed = false;

    std::thread task([&] {
        try {
            result = run_once(filename, dpn, csn, phase, restart_config, stop_flag, proof_path);
        } catch (...) {
            err = std::current_exception();
        }
        {
            std::lock_guard<std::mutex> lock(mtx);
            completed = true;
        }
        cv.notify_one();
    });

    {
        std::unique_lock<std::mutex> lock(mtx);
        if (!cv.wait_for(lock, std::chrono::seconds(timeout_s), [&] { return completed; })) {
            stop_flag = true;
        }
    }

    if (task.joinable()) {
        task.join();
    }

    if (err) {
        try {
            std::rethrow_exception(err);
        } catch (const std::exception& e) {
            std::cerr << e.what() << std::endl;
        }
        return std::nullopt;
    }

    return result;
}

/// @brief Return the current UTC timestamp in ISO-like format.
std::string utc_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &now_time_t);
#else
    gmtime_r(&now_time_t, &tm_utc);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

/// @brief Create a unique run id by combining timestamp and epoch millis.
std::string run_id_from_timestamp(const std::string& timestamp_utc) {
    auto now = std::chrono::system_clock::now();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::ostringstream ss;
    ss << timestamp_utc << "-" << millis;
    return ss.str();
}

/// @brief Entry point for the solver CLI.
int main(int argc, char *argv[]) {
    bool help{false};
    bool verbose{false};
    uint threads{4};
    heuristics::DecisionProcedureName dpn{heuristics::DecisionProcedureName::fixed_unit};
    heuristics::ClauseSelectorName csn{heuristics::ClauseSelectorName::cyclic};
    heuristics::DecisionPhase phase{heuristics::DecisionPhase::unsat};
    uint seconds{10};
    StatsMode stats_mode{StatsMode::console};
    std::string stats_file;
    std::string proof_path;
    uint runs{5};
    bool all_decproc{false};
    bool all_clausesel{false};
    search::RestartConfig restart_config{};

    enum OptionId {
        OPT_THREADS = 1000,
        OPT_RESTART
    };

    static constexpr struct option long_options[] = {
        {"decision-proc", required_argument, 0, 'd'},
        {"decision-phase", required_argument, 0, 'p'},
        {"stats-file", required_argument, 0, 's'},
        {"clause-selector", required_argument, 0, 'c'},
        {"timeout", required_argument, 0, 't'},
        {"threads", required_argument, 0, OPT_THREADS},
        {"restart", required_argument, 0, OPT_RESTART},
        {"runs", required_argument, 0, 'r'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int option_index{0};
    while (true) {
        const int option = getopt_long(argc, argv, "d:p:s:c:t:r:vh", long_options, &option_index);
        if (option == -1) {
            break;
        }
        switch (option) {
            case 'd': {
                std::string optarg_str{optarg};
                if (optarg_str == "all") {
                    all_decproc = true;
                } else if (optarg_str == "fixed_unit") {
                    all_decproc = false;
                    dpn = heuristics::DecisionProcedureName::fixed_unit;
                } else if (optarg_str == "random_clause_sum") {
                    all_decproc = false;
                    dpn = heuristics::DecisionProcedureName::random_clause_sum;
                } else if (optarg_str == "vsids_unit") {
                    all_decproc = false;
                    dpn = heuristics::DecisionProcedureName::vsids_unit; 
                } else if (optarg_str == "vsids_clause") {
                    all_decproc = false;
                    dpn = heuristics::DecisionProcedureName::vsids_clause;
                } else if (optarg_str == "cvsids") {
                    all_decproc = false;
                    dpn = heuristics::DecisionProcedureName::cvsids;
                } else if (optarg_str == "berkmin") {
                    all_decproc = false;
                    dpn = heuristics::DecisionProcedureName::berkmin;
                } else if (optarg_str == "cbh") {
                    all_decproc = false;
                    dpn = heuristics::DecisionProcedureName::cbh;
                } else {
                    std::cerr << "Error: '" << optarg << "' is not a decision procedure" << std::endl;
                    return 1;
                }
                break;
            }
            case 'p': {
                std::string optarg_str{optarg};
                if (optarg_str == "unsat") {
                    phase = heuristics::DecisionPhase::unsat;
                } else if (optarg_str == "sat") {
                    phase = heuristics::DecisionPhase::sat;
                } else if (optarg_str == "random") {
                    phase = heuristics::DecisionPhase::random;
                } else {
                    std::cerr << "Error: '" << optarg << "' is not a decision phase" << std::endl;
                    return 1;
                }
                break;
            }
            case 's':
                stats_file = optarg;
                break;
            case 'c': {
                std::string optarg_str{optarg};
                if (optarg_str == "all") {
                    all_clausesel = true;
                } else if (optarg_str == "cyclic") {
                    all_clausesel = false;
                    csn = heuristics::ClauseSelectorName::cyclic;
                } else if (optarg_str == "lt_watchlist") {
                    all_clausesel = false;
                    csn = heuristics::ClauseSelectorName::lt_watchlist;
                } else if (optarg_str == "exact_watchlist") {
                    all_clausesel = false;
                    csn = heuristics::ClauseSelectorName::exact_watchlist;
                } else if (optarg_str == "exact_watchlist2") {
                    all_clausesel = false;
                    csn = heuristics::ClauseSelectorName::exact_watchlist2;
                } else {
                    std::cerr << "Error: '" << optarg << "' is not a clause selector" << std::endl;
                    return 1;
                }
                break;
            }
            case 't':
                try {
                    seconds = std::stoul(optarg);
                } catch (const std::exception& e) {
                    std::cerr << "Error: Cannot convert '" << optarg << "' to uint" << std::endl;
                    return 1;
                }
                break;
            case OPT_THREADS:
                try {
                    threads = std::stoul(optarg);
                } catch (const std::exception& e) {
                    std::cerr << "Error: Cannot convert '" << optarg << "' to uint" << std::endl;
                    return 1;
                }
                break;
            case OPT_RESTART: {
                restart_config = search::RestartConfig{};
                std::string method{optarg};
                if (method == "none") {
                    restart_config.method = search::RestartMethod::none;
                } else if (method == "geometric") {
                    restart_config.method = search::RestartMethod::geometric;
                    if (optind < argc) {
                        if (auto maybe_rate = parse_double_token(argv[optind]); maybe_rate.has_value()) {
                            restart_config.geometric_multiplier = *maybe_rate;
                            ++optind;
                        }
                    }
                    if (optind < argc) {
                        if (auto maybe_initial = parse_size_t_token(argv[optind]); maybe_initial.has_value()) {
                            restart_config.geometric_initial_conflicts = *maybe_initial;
                            ++optind;
                        }
                    }
                    if (!std::isfinite(restart_config.geometric_multiplier) ||
                        restart_config.geometric_multiplier < 1.0) {
                        std::cerr << "Error: geometric restart rate_increase must be finite and >= 1.0" << std::endl;
                        return 1;
                    }
                    if (restart_config.geometric_initial_conflicts < 1) {
                        std::cerr << "Error: geometric restart initial_conflicts must be >= 1" << std::endl;
                        return 1;
                    }
                } else if (method == "luby") {
                    restart_config.method = search::RestartMethod::luby;
                    if (optind < argc) {
                        if (auto maybe_unit = parse_size_t_token(argv[optind]); maybe_unit.has_value()) {
                            restart_config.luby_unit_conflicts = *maybe_unit;
                            ++optind;
                        }
                    }
                    if (restart_config.luby_unit_conflicts < 1) {
                        std::cerr << "Error: luby restart unit_conflicts must be >= 1" << std::endl;
                        return 1;
                    }
                } else {
                    std::cerr << "Error: '" << method << "' is not a restart method" << std::endl;
                    return 1;
                }
                break;
            }
            case 'r':
                try {
                    runs = std::stoul(optarg);
                } catch (const std::exception& e) {
                    std::cerr << "Error: Cannot convert '" << optarg << "' to uint" << std::endl;
                    return 1;
                }
                if (runs == 0) {
                    std::cerr << "Error: runs must be >= 1" << std::endl;
                    return 1;
                }
                break;
            case 'v':
                verbose = true;
                break;
            case 'h':
                help = true;
                break;
            default:
                std::cerr << "Usage: " << argv[0] << " [options] <input> [proof_path]" << std::endl;
                return 1;
        }
    }

    if (help) {
        std::cout << "Usage: " << argv[0] << " [options] <input> [proof_path]" << std::endl;
        std::cout << "Options:" << std::endl;
        std::cout << "  -d, --decision-proc <proc>   Decision procedure (default: fixed_unit)" << std::endl;
        std::cout << "                               Options: fixed_unit, random_clause_sum, vsids_unit, vsids_clause, cvsids, berkmin, cbh, all" << std::endl;
        std::cout << "  -p, --decision-phase <phase> Decision phase for clause-based decisions (default: unsat)" << std::endl;
        std::cout << "                               Options: unsat, sat, random" << std::endl;
        std::cout << "  -s, --stats-file <path>      CSV output path (implies CSV stats format)" << std::endl;
        std::cout << "  -c, --clause-selector <sel>  Clause selector (default: cyclic)" << std::endl;
        std::cout << "                               Options: cyclic, lt_watchlist, exact_watchlist, exact_watchlist2, all" << std::endl;
        std::cout << "  -t, --timeout <s>            Timeout in seconds (default: 10)" << std::endl;
        std::cout << "      --threads <n>            Number of threads (default: 4)" << std::endl;
        std::cout << "      --restart <method> [rate] [arg2]" << std::endl;
        std::cout << "                               Restart policy (default: none)" << std::endl;
        std::cout << "                               none" << std::endl;
        std::cout << "                               geometric [rate_increase>=1.0] [initial_conflicts>=1] (defaults: 1.5, 100)" << std::endl;
        std::cout << "                               luby [unit_conflicts>=1] (default: 100)" << std::endl;
        std::cout << "  -r, --runs <n>               Runs for nondeterministic settings (random_clause_sum, vsids_clause, cvsids, berkmin, cbh; median selected, default: 5)" << std::endl;
        std::cout << "  -v, --verbose                Verbose mode (default: false)" << std::endl;
        std::cout << "  -h, --help                   Show this help message" << std::endl;
        std::cout << "Notes:" << std::endl;
        std::cout << "  proof_path is an optional second positional argument for DRUP(⊕) output." << std::endl;
        
        return 0;
    }

    std::vector<std::string> positional_args;
    for (int i = optind; i < argc; ++i) {
        positional_args.emplace_back(argv[i]);
    }
    if (positional_args.empty() || positional_args.size() > 2) {
        std::cerr << "Usage: " << argv[0] << " [options] <input> [proof_path]" << std::endl;
        return 1;
    }

    const std::string input_path = positional_args[0];
    if (positional_args.size() == 2) {
        proof_path = positional_args[1];
    }

    if (!fs::exists(input_path)) {
        std::cerr << "Error: input does not exist: " << input_path << std::endl;
        return 1;
    }

    const bool input_is_dir = fs::is_directory(input_path);
    if (!input_is_dir && !fs::is_regular_file(input_path)) {
        std::cerr << "Error: input is neither a file nor a directory: " << input_path << std::endl;
        return 1;
    }

    if (!proof_path.empty()) {
        if (input_is_dir) {
            std::cerr << "Proof logging requires a single input file (no directory mode)." << std::endl;
            return 1;
        }
        if (all_decproc || all_clausesel) {
            std::cerr << "Proof logging does not support 'all' decision procedures or clause selectors." << std::endl;
            return 1;
        }
        if (runs != 1) {
            std::cerr << "Proof logging requires --runs 1." << std::endl;
            return 1;
        }
    }

    stats_mode = stats_file.empty() ? StatsMode::console : StatsMode::csv;

    if (verbose && stats_mode == StatsMode::console) {
        std::cout << "Verbose mode enabled" << std::endl;
    }

    std::vector<std::string> files;

    if (!input_is_dir) {
        files.push_back(input_path);
    } else {
        for (auto const& entry : fs::recursive_directory_iterator(input_path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".cnf") {
                files.push_back(fs::relative(entry.path(), fs::current_path()).string());
            }
        }
        std::sort(files.begin(), files.end());
    }

    std::shared_ptr<stats::StatsSink> sink;
    if (stats_mode == StatsMode::csv) {
        sink = std::make_shared<stats::CsvStatsSink>(stats_file);
    } else {
        sink = std::make_shared<stats::ConsoleStatsSink>();
    }

    const std::string timestamp_utc = utc_timestamp();
    const std::string run_id = run_id_from_timestamp(timestamp_utc);

    std::vector<heuristics::DecisionProcedureName> dp_list;
    if (all_decproc) {
        dp_list = {
            heuristics::DecisionProcedureName::fixed_unit,
            heuristics::DecisionProcedureName::random_clause_sum,
            heuristics::DecisionProcedureName::vsids_unit,
            heuristics::DecisionProcedureName::vsids_clause,
            heuristics::DecisionProcedureName::cvsids,
            heuristics::DecisionProcedureName::berkmin,
            heuristics::DecisionProcedureName::cbh
        };
    } else {
        dp_list = {dpn};
    }

    std::vector<heuristics::ClauseSelectorName> cs_list;
    if (all_clausesel) {
        cs_list = {
            heuristics::ClauseSelectorName::cyclic,
            heuristics::ClauseSelectorName::lt_watchlist,
            heuristics::ClauseSelectorName::exact_watchlist,
            heuristics::ClauseSelectorName::exact_watchlist2
        };
    } else {
        cs_list = {csn};
    }

    /// @brief Unit of work combining an input file and heuristic choices.
    struct WorkItem {
        std::string file; ///< Input file path.
        heuristics::DecisionProcedureName dpn; ///< Decision procedure to run.
        heuristics::ClauseSelectorName csn; ///< Clause selector to run.
    };

    std::vector<WorkItem> work_items;
    work_items.reserve(files.size() * dp_list.size() * cs_list.size());
    for (const auto& file : files) {
        for (const auto& dp : dp_list) {
            for (const auto& cs : cs_list) {
                work_items.push_back(WorkItem{file, dp, cs});
            }
        }
    }

    std::vector<std::optional<stats::AggregateStats>> results(work_items.size());
    std::atomic<std::size_t> next_idx{0};
    
    // Create worker threads
    std::vector<std::thread> workers;
    for (int i = 0; i < threads; ++i) {
        workers.emplace_back([&] {
            while (true) {
                std::size_t idx = next_idx.fetch_add(1);
                if (idx >= work_items.size()) {
                    break;
                }

                const auto& item = work_items[idx];
                
                std::size_t runs_requested = stats::is_nondeterministic(item.dpn) ? runs : 1;
                bool deterministic = !stats::is_nondeterministic(item.dpn);
                std::vector<stats::RunStats> run_stats;
                run_stats.reserve(runs_requested);

                for (std::size_t r = 0; r < runs_requested; ++r) {
                    auto maybe_stats = run_once_with_timeout(
                        item.file, item.dpn, item.csn, phase, restart_config, seconds, proof_path);
                    if (!maybe_stats.has_value()) {
                        run_stats.clear();
                        break;
                    }
                    run_stats.push_back(*maybe_stats);
                }

                if (run_stats.empty()) {
                    continue;
                }

                stats::AggregateMeta meta;
                meta.run_id = run_id;
                meta.timestamp_utc = timestamp_utc;
                meta.file = item.file;
                meta.decision_procedure = item.dpn;
                meta.decision_phase = phase;
                meta.clause_selector = item.csn;
                meta.threads = threads;
                meta.timeout_s = seconds;
                meta.deterministic = deterministic;
                meta.runs_requested = runs_requested;

                results[idx] = stats::aggregate_runs(meta, run_stats);
            }
        });
    }
    
    // Wait for all workers to finish
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    for (const auto& result : results) {
        if (result.has_value()) {
            sink->log(*result);
        }
    }

    return 0;
}
