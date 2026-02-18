#include <future>
#include <filesystem>
#include <thread>
#include <getopt.h>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <atomic>
#include <format>
#include <mutex>
#include <vector>
#include <algorithm>
#include "search/Solver.h"
#include "heuristics/DecisionProcedure.h"
#include "heuristics/ClauseSelector.h"
#include "util/Stats.h"
#include "util/StatsLogger.h"

namespace fs = std::filesystem;

enum class StatsMode { console, csv };

stats::RunStats run_once(const std::string& filename,
                         heuristics::DecisionProcedureName dpn,
                         heuristics::ClauseSelectorName csn,
                         std::atomic<bool>& stop_flag) {
    search::Solver solver = search::Solver::from_DIMACS(filename, dpn, csn);
    std::size_t num_vars = solver.get_num_vars();
    std::size_t start_num_clauses = solver.get_num_clauses();

    auto start = std::chrono::high_resolution_clock::now();
    auto result = solver.solve(stop_flag);
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;

    stats::RunStats out;
    out.num_vars = num_vars;
    out.start_clauses = start_num_clauses;
    out.final_clauses = solver.get_num_clauses();
    out.time_s = duration.count();
    out.num_conflicts = solver.num_conflicts;
    out.num_propagations = solver.num_propagations;
    out.num_attempt_props = solver.num_attempt_props;
    out.num_watch_updates = solver.num_watch_updates;
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

std::optional<stats::RunStats> run_once_with_timeout(const std::string& filename,
                                                     heuristics::DecisionProcedureName dpn,
                                                     heuristics::ClauseSelectorName csn,
                                                     std::size_t timeout_s) {
    std::optional<stats::RunStats> result;
    std::exception_ptr err;
    std::atomic<bool> stop_flag(false);
    std::mutex mtx;
    std::condition_variable cv;
    bool completed = false;

    std::thread task([&] {
        try {
            result = run_once(filename, dpn, csn, stop_flag);
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

std::string run_id_from_timestamp(const std::string& timestamp_utc) {
    auto now = std::chrono::system_clock::now();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::ostringstream ss;
    ss << timestamp_utc << "-" << millis;
    return ss.str();
}

int main(int argc, char *argv[]) {
    std::string filename;
    std::string directory;
    bool help{false};
    bool verbose{false};
    uint threads{4};
    heuristics::DecisionProcedureName dpn{heuristics::DecisionProcedureName::fixed_unit};
    heuristics::ClauseSelectorName csn{heuristics::ClauseSelectorName::cyclic};
    uint seconds{10};
    StatsMode stats_mode{StatsMode::console};
    std::string csv_path;
    uint runs{5};
    bool all_decproc{false};
    bool all_clausesel{false};

    static constexpr struct option long_options[] = {
        {"file", required_argument, 0, 'f'},
        {"directory", required_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {"verbose", no_argument, 0, 'v'},
        {"threads", required_argument, 0, 't'},
        {"decproc", required_argument, 0, 'p'},
        {"clausesel", required_argument, 0, 'c'},
        {"seconds", required_argument, 0, 's'},
        {"stats-mode", required_argument, 0, 'm'},
        {"csv", required_argument, 0, 'o'},
        {"runs", required_argument, 0, 'r'},
        {0, 0, 0, 0}
    };

    int option_index{0};
    while (true) {
        const int option = getopt_long(argc, argv, "f:d:hvt:p:c:s:m:o:r:", long_options, &option_index);
        if (option == -1) {
            break;
        }
        switch (option) {
            case 'f':
                filename = optarg;
                break;
            case 'd':
                directory = optarg;
                break;
            case 'h':
                help = true;
                break;
            case 'v':
                verbose = true;
                break;
            case 't':
                try {
                    threads = std::stoul(optarg);
                } catch (const std::exception& e) {
                    std::cerr << "Error: Cannot convert '" << optarg << "' to uint" << std::endl;
                    return 1;
                }
                break;
            case 'p': {
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
                } else {
                    std::cerr << "Error: '" << optarg << "' is not a clause selector" << std::endl;
                    return 1;
                }
                break;
            }
            case 's':
                try {
                    seconds = std::stoul(optarg);
                } catch (const std::exception& e) {
                    std::cerr << "Error: Cannot convert '" << optarg << "' to uint" << std::endl;
                    return 1;
                }
                break;
            case 'm': {
                std::string mode{optarg};
                if (mode == "console") {
                    stats_mode = StatsMode::console;
                } else if (mode == "csv") {
                    stats_mode = StatsMode::csv;
                } else {
                    std::cerr << "Error: '" << optarg << "' is not a stats mode" << std::endl;
                    return 1;
                }
                break;
            }
            case 'o':
                csv_path = optarg;
                break;
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
            default:
                std::cerr << "Usage: " << argv[0] << " [-f <file> | -d <directory>] [options]" << std::endl;
                return 1;
        }
    }

    if (help) {
        std::cout << "Usage: " << argv[0] << " [-f <file> | -d <directory>] [options]" << std::endl;
        std::cout << "Options:" << std::endl;
        std::cout << "  -f, --file <filename>        Specify the CNF file" << std::endl;
        std::cout << "  -d, --directory <directory>  Process all .cnf files in the directory" << std::endl;
        std::cout << "  -h, --help                   Show this help message" << std::endl;
        std::cout << "  -v, --verbose                Verbose mode (default: false)" << std::endl;
        std::cout << "  -t, --threads <n>            Number of threads (default: 4)" << std::endl;
        std::cout << "  -p, --decproc <proc>         Decision procedure (default: fixed_unit)" << std::endl;
        std::cout << "                               Options: fixed_unit, random_clause_sum, vsids_unit, vsids_clause, cvsids, berkmin, cbh, all" << std::endl;
        std::cout << "  -c, --clausesel <selector>   Clause selector (default: cyclic)" << std::endl;
        std::cout << "                               Options: cyclic, lt_watchlist, exact_watchlist, all" << std::endl;
        std::cout << "  -s, --seconds <s>            Timeout in seconds (default: 10)" << std::endl;
        std::cout << "  -m, --stats-mode <mode>      Stats output: console or csv (default: console)" << std::endl;
        std::cout << "  -o, --csv <path>             CSV output path when stats-mode=csv (default: stats.csv)" << std::endl;
        std::cout << "  -r, --runs <n>               Runs for nondeterministic settings (random_clause_sum, vsids_clause, cvsids, berkmin, cbh; median selected, default: 5)" << std::endl;
        
        return 0;
    }

    if ((filename.empty() && directory.empty()) || (!filename.empty() && !directory.empty())) {
        std::cerr << "Exactly one of filename or directory is required. Usage: " << argv[0] << " [-f <file> | -d <directory>] [options]" << std::endl;
        return 1;
    }

    if (verbose && stats_mode == StatsMode::console) {
        std::cout << "Verbose mode enabled" << std::endl;
    }

    std::vector<std::string> files;

    if (!filename.empty()) {
        if (!fs::exists(filename)) {
            std::cerr << "Error: file does not exist: " << filename << std::endl;
            return 1;
        }
        if (!fs::is_regular_file(filename)) {
            std::cerr << "Error: " << filename << " is not a regular file" << std::endl;
            return 1;
        }
        files.push_back(filename);
    } else {
        if (!fs::exists(directory)) {
            std::cerr << "Error: directory does not exist: " << directory << std::endl;
            return 1;
        }
        if (!fs::is_directory(directory)) {
            std::cerr << "Error: " << directory << " is not a directory" << std::endl;
            return 1;
        }

        for (auto const& entry : fs::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file() && entry.path().extension() == ".cnf") {
                files.push_back(fs::relative(entry.path(), fs::current_path()).string());
            }
        }
        std::sort(files.begin(), files.end());
    }

    if (stats_mode == StatsMode::csv && csv_path.empty()) {
        csv_path = "stats.csv";
    }

    std::shared_ptr<stats::StatsSink> sink;
    if (stats_mode == StatsMode::csv) {
        sink = std::make_shared<stats::CsvStatsSink>(csv_path);
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
            heuristics::ClauseSelectorName::exact_watchlist
        };
    } else {
        cs_list = {csn};
    }

    struct WorkItem {
        std::string file;
        heuristics::DecisionProcedureName dpn;
        heuristics::ClauseSelectorName csn;
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
                    auto maybe_stats = run_once_with_timeout(item.file, item.dpn, item.csn, seconds);
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
