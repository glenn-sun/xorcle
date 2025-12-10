#include <future>
#include <filesystem>
#include <thread>
#include <getopt.h>
#include "Solver.h"

namespace fs = std::filesystem;

void solve(std::string& filename, DecisionProcedureName dpn, std::atomic<bool>& stop_flag) {
    Solver solver = Solver::from_DIMACS(filename, dpn);
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
    ss << "Time: " << std::format("{:.{}g}", time / 1000000000, 3) << " s" << std::endl;
    if (ResultCase::is_timeout(result)) {
        ss << "Timeout" << std::endl;
    } else if (ResultCase::is_unsat(result)) {
        ss << "Unsatisfiable" << std::endl;
    } else if (ResultCase::is_sat(result)) {
        ss << "Satisfiable: " << ResultCase::unwrap_sat(result) << std::endl;
    }
    ss << std::endl;

    std::cout << ss.str();
}

int main(int argc, char *argv[]) {
    std::string filename;
    std::string directory;
    bool help{false};
    bool verbose{false};
    uint threads{4};
    DecisionProcedureName dpn{DecisionProcedureName::fixed_unit};
    uint seconds{10};

    static constexpr struct option long_options[] = {
        {"file", required_argument, 0, 'f'},
        {"directory", required_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {"verbose", no_argument, 0, 'v'},
        {"threads", required_argument, 0, 't'},
        {"decproc", required_argument, 0, 'p'},
        {"seconds", required_argument, 0, 's'},
        {0, 0, 0, 0}
    };

    int option_index{0};
    while (true) {
        const int option = getopt_long(argc, argv, "f:d:hvt:p:s:", long_options, &option_index);
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
                    dpn = DecisionProcedureName::fixed_unit;
                } else if (optarg_str == "random_unit") {
                    dpn = DecisionProcedureName::random_unit;
                } else if (optarg_str == "random_eq") {
                    dpn = DecisionProcedureName::random_eq;
                } else if (optarg_str == "vsids_unit") {
                    dpn = DecisionProcedureName::vsids_unit; 
                } else {
                    std::cerr << "Error: '" << optarg << "' is not a decision procedure" << std::endl;
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
        std::cout << "                               Options: fixed_unit, random_unit, random_eq" << std::endl;
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
                    solve(filename, dpn, stop_flag);
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
