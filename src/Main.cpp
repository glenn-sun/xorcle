#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <getopt.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
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

std::optional<bool> parse_bool_token(const char* token) {
    if (token == nullptr) {
        return std::nullopt;
    }
    std::string s(token);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "1" || s == "true" || s == "on" || s == "yes") {
        return true;
    }
    if (s == "0" || s == "false" || s == "off" || s == "no") {
        return false;
    }
    return std::nullopt;
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
                         const heuristics::CVsidsConfig& cvsids_config,
                         const search::RestartConfig& restart_config,
                         bool enable_compaction,
                         std::size_t compaction_conflict_interval,
                         std::atomic<bool>& stop_flag,
                         const std::string& proof_path = {}) {
    std::shared_ptr<io::ProofLogger> proof_logger;
    if (!proof_path.empty()) {
        proof_logger = std::make_shared<io::ProofLogger>(proof_path);
    }
    search::Solver solver =
        search::Solver::from_DIMACS(filename, dpn, csn, phase, cvsids_config, proof_logger, restart_config,
                                    enable_compaction, compaction_conflict_interval);
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
    heuristics::DecisionProcedureName dpn{heuristics::DecisionProcedureName::cvsids};
    heuristics::CVsidsConfig cvsids_config{};
    heuristics::ClauseSelectorName csn{heuristics::ClauseSelectorName::cyclic};
    heuristics::DecisionPhase phase{heuristics::DecisionPhase::unsat};
    StatsMode stats_mode{StatsMode::console};
    std::string stats_file;
    std::string proof_path;
    search::RestartConfig restart_config{};
    bool enable_compaction = true;
    std::size_t compaction_conflict_interval = 16;

    static constexpr struct option long_options[] = {
        {"decision-proc", required_argument, 0, 'd'},
        {"decision-phase", required_argument, 0, 'p'},
        {"stats-file", required_argument, 0, 's'},
        {"clause-selector", required_argument, 0, 'c'},
        {"restart", required_argument, 0, 'r'},
        {"compact", required_argument, 0, 'x'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int option_index{0};
    while (true) {
        const int option = getopt_long(argc, argv, "d:p:s:c:r:x:vh", long_options, &option_index);
        if (option == -1) {
            break;
        }
        switch (option) {
            case 'd': {
                std::string optarg_str{optarg};
                if (optarg_str == "fixed_unit") {
                    dpn = heuristics::DecisionProcedureName::fixed_unit;
                } else if (optarg_str == "random_clause_sum") {
                    dpn = heuristics::DecisionProcedureName::random_clause_sum;
                } else if (optarg_str == "vsids_unit") {
                    dpn = heuristics::DecisionProcedureName::vsids_unit; 
                } else if (optarg_str == "vsids_clause") {
                    dpn = heuristics::DecisionProcedureName::vsids_clause;
                } else if (optarg_str == "cvsids") {
                    dpn = heuristics::DecisionProcedureName::cvsids;
                    cvsids_config = heuristics::CVsidsConfig{};
                    if (optind < argc) {
                        if (auto maybe_base = parse_double_token(argv[optind]); maybe_base.has_value()) {
                            if (optind + 2 >= argc) {
                                std::cerr << "Error: -d cvsids optional parameters require exactly 3 values: "
                                             "[base decay bump]" << std::endl;
                                return 1;
                            }
                            auto maybe_decay = parse_double_token(argv[optind + 1]);
                            auto maybe_bump = parse_double_token(argv[optind + 2]);
                            if (!maybe_decay.has_value() || !maybe_bump.has_value()) {
                                std::cerr << "Error: -d cvsids optional parameters require exactly 3 numeric values: "
                                             "[base decay bump]" << std::endl;
                                return 1;
                            }
                            cvsids_config.learned_base = *maybe_base;
                            cvsids_config.decay = *maybe_decay;
                            cvsids_config.bump = *maybe_bump;
                            optind += 3;
                        }
                    }
                } else if (optarg_str == "berkmin") {
                    dpn = heuristics::DecisionProcedureName::cvsids;
                    cvsids_config = heuristics::CVsidsConfig{1.0, 0.5, 0.0};
                    if (optind < argc && parse_double_token(argv[optind]).has_value()) {
                        std::cerr << "Error: -d berkmin is a fixed alias and does not accept [base decay bump]"
                                  << std::endl;
                        return 1;
                    }
                } else if (optarg_str == "cmtf") {
                    dpn = heuristics::DecisionProcedureName::cvsids;
                    cvsids_config = heuristics::CVsidsConfig{1.0, 0.25, 1.0};
                    if (optind < argc && parse_double_token(argv[optind]).has_value()) {
                        std::cerr << "Error: -d cmtf is a fixed alias and does not accept [base decay bump]"
                                  << std::endl;
                        return 1;
                    }
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
                if (optarg_str == "cyclic") {
                    csn = heuristics::ClauseSelectorName::cyclic;
                } else if (optarg_str == "lt_watchlist") {
                    csn = heuristics::ClauseSelectorName::lt_watchlist;
                } else if (optarg_str == "exact_watchlist") {
                    csn = heuristics::ClauseSelectorName::exact_watchlist;
                } else if (optarg_str == "exact_watchlist2") {
                    // Backward-compatible alias: EW2 was renamed to exact_watchlist.
                    csn = heuristics::ClauseSelectorName::exact_watchlist;
                } else {
                    std::cerr << "Error: '" << optarg << "' is not a clause selector" << std::endl;
                    return 1;
                }
                break;
            }
            case 'r': {
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
            case 'v':
                verbose = true;
                break;
            case 'x': {
                auto maybe_bool = parse_bool_token(optarg);
                if (!maybe_bool.has_value()) {
                    std::cerr << "Error: --compact expects on|off" << std::endl;
                    return 1;
                }
                enable_compaction = *maybe_bool;
                if (optind < argc) {
                    auto maybe_interval = parse_size_t_token(argv[optind]);
                    if (maybe_interval.has_value()) {
                        if (!enable_compaction) {
                            std::cerr << "Error: --compact off does not accept a conflicts interval" << std::endl;
                            return 1;
                        }
                        if (*maybe_interval < 1) {
                            std::cerr << "Error: --compact conflicts interval must be >= 1" << std::endl;
                            return 1;
                        }
                        compaction_conflict_interval = *maybe_interval;
                        ++optind;
                    } else if (enable_compaction) {
                        const std::string next_token = argv[optind];
                        if (!next_token.empty() && next_token[0] != '-') {
                            const bool looks_like_path =
                                (next_token.find('/') != std::string::npos)
                                || (next_token.find('.') != std::string::npos)
                                || fs::exists(next_token);
                            if (!looks_like_path) {
                                std::cerr << "Error: --compact conflicts interval must be an integer >= 1" << std::endl;
                                return 1;
                            }
                        }
                    }
                }
                break;
            }
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
        std::cout << "  -d, --decision-proc <proc>   Decision procedure (default: cvsids)" << std::endl;
        std::cout << "                               Options: fixed_unit, random_clause_sum, vsids_unit, "
                     "vsids_clause, cvsids, berkmin, cmtf" << std::endl;
        std::cout << "                               Optional only for cvsids: [base decay bump]" << std::endl;
        std::cout << "                               berkmin alias => cvsids 1 0.5 0" << std::endl;
        std::cout << "                               cmtf alias => cvsids 1 0.25 1" << std::endl;
        std::cout << "  -p, --decision-phase <phase> Decision phase for clause-based decisions (default: unsat)" << std::endl;
        std::cout << "                               Options: unsat, sat, random" << std::endl;
        std::cout << "  -s, --stats-file <path>      CSV output path (implies CSV stats format)" << std::endl;
        std::cout << "  -c, --clause-selector <sel>  Clause selector (default: cyclic)" << std::endl;
        std::cout << "                               Options: cyclic, lt_watchlist, exact_watchlist" << std::endl;
        std::cout << "  -r, --restart <method> [rate] [arg2]" << std::endl;
        std::cout << "                               Restart policy (default: none)" << std::endl;
        std::cout << "                               none" << std::endl;
        std::cout << "                               geometric [rate_increase>=1.0] [initial_conflicts>=1] (defaults: 1.5, 100)" << std::endl;
        std::cout << "                               luby [unit_conflicts>=1] (default: 100)" << std::endl;
        std::cout << "  -x, --compact <on|off> [conflicts>=1]  Compaction (default: on, conflicts: 16)"
                  << std::endl;
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

    if (!fs::is_regular_file(input_path)) {
        std::cerr << "Error: input must be a regular file: " << input_path << std::endl;
        return 1;
    }

    stats_mode = stats_file.empty() ? StatsMode::console : StatsMode::csv;

    if (dpn == heuristics::DecisionProcedureName::cvsids) {
        if (!std::isfinite(cvsids_config.learned_base) || cvsids_config.learned_base < 0.0) {
            std::cerr << "Error: cvsids base must be finite and >= 0" << std::endl;
            return 1;
        }
        if (!std::isfinite(cvsids_config.bump) || cvsids_config.bump < 0.0) {
            std::cerr << "Error: cvsids bump must be finite and >= 0" << std::endl;
            return 1;
        }
        if (!std::isfinite(cvsids_config.decay) || cvsids_config.decay <= 0.0 || cvsids_config.decay >= 1.0) {
            std::cerr << "Error: cvsids decay must be finite and satisfy 0 < decay < 1" << std::endl;
            return 1;
        }
    }

    if (verbose && stats_mode == StatsMode::console) {
        std::cout << "Verbose mode enabled" << std::endl;
    }

    std::shared_ptr<stats::StatsSink> sink;
    if (stats_mode == StatsMode::csv) {
        sink = std::make_shared<stats::CsvStatsSink>(stats_file);
    } else {
        sink = std::make_shared<stats::ConsoleStatsSink>();
    }

    const std::string timestamp_utc = utc_timestamp();
    const std::string run_id = run_id_from_timestamp(timestamp_utc);

    const bool deterministic = !stats::is_nondeterministic(dpn);
    std::vector<stats::RunStats> run_stats;
    run_stats.reserve(1);
    try {
        std::atomic<bool> stop_flag(false);
        run_stats.push_back(run_once(input_path, dpn, csn, phase, cvsids_config, restart_config,
                                     enable_compaction, compaction_conflict_interval, stop_flag, proof_path));
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    stats::AggregateMeta meta;
    meta.run_id = run_id;
    meta.timestamp_utc = timestamp_utc;
    meta.file = input_path;
    meta.decision_procedure = dpn;
    meta.decision_phase = phase;
    meta.clause_selector = csn;
    meta.threads = 1;
    meta.timeout_s = 0;
    meta.deterministic = deterministic;
    meta.runs_requested = 1;
    if (dpn == heuristics::DecisionProcedureName::cvsids) {
        meta.cvsids_learned_base = cvsids_config.learned_base;
        meta.cvsids_decay = cvsids_config.decay;
        meta.cvsids_bump = cvsids_config.bump;
    }
    sink->log(stats::aggregate_runs(meta, run_stats));

    return 0;
}
