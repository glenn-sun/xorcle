#include <future>
#include <filesystem>
#include <thread>
#include <getopt.h>
#include "search/Solver.h"
#include "heuristics/DecisionProcedure.h"
#include "heuristics/ClauseSelector.h"

namespace fs = std::filesystem;

void solve(std::string& filename, heuristics::DecisionProcedureName dpn, heuristics::ClauseSelectorName csn, std::atomic<bool>& stop_flag) {
    try {
        search::Solver solver = search::Solver::from_DIMACS(filename, dpn, csn);
        int num_vars = solver.get_num_vars();
        int start_num_clauses = solver.get_num_clauses();

        auto start = std::chrono::high_resolution_clock::now();
        auto result = solver.solve(stop_flag);
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::nano> duration = end - start;
        auto time = duration.count();

        std::stringstream ss;
        ss << "File: " << filename << std::endl;
        ss << "Number of variables: " << num_vars << std::endl;
        ss << "Number of clauses: " << start_num_clauses << std::endl;
        ss << "Final number of clauses: " << solver.get_num_clauses() << std::endl;
        ss << "Propagations per conflict: " << (double) solver.num_propagations / solver.num_conflicts << std::endl;
        ss << "Attempts per propagation: " << (double) solver.num_attempt_props / solver.num_propagations << std::endl;
        ss << "Attempts per watch update: " << (double) solver.num_attempt_props / solver.num_watch_updates << std::endl;
        auto stats = solver.decision_stats();
        double fallback_pct = stats.decisions == 0 ? 0.0 : 100.0 * static_cast<double>(stats.fallback_decisions) / static_cast<double>(stats.decisions);
        ss << "Decision fallback: " << std::format("{:.{}g}", fallback_pct, 3) << "% (" << stats.fallback_decisions << "/" << stats.decisions << ")" << std::endl;
        ss << "Time: " << std::format("{:.{}g}", time / 1000000000, 3) << " s" << std::endl;
        if (search::ResultCase::is_timeout(result)) {
            ss << "Timeout" << std::endl;
        } else if (search::ResultCase::is_unsat(result)) {
            ss << "Unsatisfiable" << std::endl;
        } else if (search::ResultCase::is_sat(result)) {
            ss << "Satisfiable: " << search::ResultCase::unwrap_sat(result) << std::endl;
        }
        ss << std::endl;

        std::cout << ss.str();
    } catch (const std::runtime_error& e) {
        std::cerr << e.what() << std::endl;
    }
    
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

    static constexpr struct option long_options[] = {
        {"file", required_argument, 0, 'f'},
        {"directory", required_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {"verbose", no_argument, 0, 'v'},
        {"threads", required_argument, 0, 't'},
        {"decproc", required_argument, 0, 'p'},
        {"clausesel", required_argument, 0, 'c'},
        {"seconds", required_argument, 0, 's'},
        {0, 0, 0, 0}
    };

    int option_index{0};
    while (true) {
        const int option = getopt_long(argc, argv, "f:d:hvt:p:c:s:", long_options, &option_index);
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
                } else if (optarg_str == "berkmin") {
                    dpn = heuristics::DecisionProcedureName::berkmin;
                } else if (optarg_str == "cbh") {
                    dpn = heuristics::DecisionProcedureName::cbh;
                } else {
                    std::cerr << "Error: '" << optarg << "' is not a decision procedure" << std::endl;
                    return 1;
                }
                break;
            }
            case 'c': {
                std::string optarg_str{optarg};
                if (optarg_str == "cyclic") {
                    csn = heuristics::ClauseSelectorName::cyclic;
                } else if (optarg_str == "lt_watchlist") {
                    csn = heuristics::ClauseSelectorName::lt_watchlist;
                } else if (optarg_str == "exact_watchlist") {
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
        std::cout << "                               Options: fixed_unit, random_clause_sum, vsids_unit, vsids_clause, cvsids, berkmin, cbh" << std::endl;
        std::cout << "  -c, --clausesel <selector>   Clause selector (default: cyclic)" << std::endl;
        std::cout << "                               Options: cyclic, lt_watchlist, exact_watchlist" << std::endl;
        std::cout << "  -s, --seconds <s>            Timeout in seconds (default: 10)" << std::endl;
        
        return 0;
    }

    if ((filename.empty() && directory.empty()) || (!filename.empty() && !directory.empty())) {
        std::cerr << "Exactly one of filename or directory is required. Usage: " << argv[0] << " [-f <file> | -d <directory>] [options]" << std::endl;
        return 1;
    }

    if (verbose) {
        std::cout << "Verbose mode enabled" << std::endl;
    }

    std::queue<std::string> tasks;

    if (!filename.empty()) {
        if (!fs::exists(filename)) {
            std::cerr << "Error: file does not exist: " << filename << std::endl;
            return 1;
        }
        if (!fs::is_regular_file(filename)) {
            std::cerr << "Error: " << filename << " is not a regular file" << std::endl;
            return 1;
        }
        tasks.push(filename);
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
                tasks.push(fs::relative(entry.path(), fs::current_path()).string());
            }
        }
    }

    // Create mutex for task queue synchronization
    std::mutex tasks_mutex;
    
    // Create worker threads
    std::vector<std::thread> workers;
    for (int i = 0; i < threads; ++i) {
        workers.emplace_back([&] {
            while (true) {
                // Get a task from the queue
                std::string filename;
                {
                    std::lock_guard<std::mutex> lock(tasks_mutex);
                    if (tasks.empty()) break;
                    filename = tasks.front();
                    tasks.pop();
                }
                
                // Set up timeout mechanism
                std::atomic<bool> stop_flag(false);
                std::mutex mtx;
                std::condition_variable cv;
                bool task_completed = false;
                
                // Run the task
                std::thread task_thread([&] {
                    solve(filename, dpn, csn, stop_flag);
                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        task_completed = true;
                    }
                    cv.notify_one();
                });
                
                // Wait for the task to complete or timeout
                {
                    std::unique_lock<std::mutex> lock(mtx);
                    if (!cv.wait_for(lock, std::chrono::seconds(seconds), [&] { return task_completed; })) {
                        // Task timed out
                        stop_flag = true;
                    }
                }
                
                // Wait for the task thread to finish
                if (task_thread.joinable()) {
                    task_thread.join();
                }
            }
        });
    }
    
    // Wait for all workers to finish
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    return 0;
}
