#include <iostream>
#include <getopt.h>
#include "Solver.h"
#include <chrono>

// void test() {
//     // Create a random 5x10 bit matrix
//     bit::matrix<uint64_t> matrix = bit::matrix<uint64_t>::random(5, 10);

//     // Compute row echelon form
//     matrix.to_reduced_echelon_form();

//     // Print the matrix in row echelon form
//     std::cout << "Row Echelon Form:" << std::endl;
//     std::cout << matrix.to_pretty_string() << std::endl;

//     // Create a random bit vector with 10 entries
//     bit::vector<uint64_t> vector = bit::vector<uint64_t>::random(10);
//     std:: cout << "Vector:" << std::endl << vector.to_pretty_string() <<    std::endl;

//     std::cout << "Span contains:" << matrix.span_contains(vector) << std::endl;
 
//     // Try to reduce the vector using the matrix in row echelon form
//     matrix.push_to_echelon_form(vector);

//     // Print the final result
//     std::cout << "Final Result:" << std::endl;
//     std::cout << matrix.to_pretty_string() << std::endl;

//     auto reduced = matrix.reduce_copy(vector);
//     std:: cout << "Reduced:" << std::endl << reduced.to_pretty_string() <<    std::endl;

//     std::cout << "Span contains:" << matrix.span_contains(vector) << std::endl;
// }

// auto generate_data() {
//     bit::matrix<> matrix = bit::matrix<>::random(25, 100);
//     matrix.to_echelon_form();

//     bit::vector<> vector = bit::vector<>::random(100);
//     return std::make_tuple(matrix, vector);
// }

// void test_contains(int N) {
//     auto total_time = 0.;

//     for (int i = 0; i < N; ++i) {
//         auto [matrix, vector] = generate_data();
//         auto start = std::chrono::high_resolution_clock::now();
        
//         auto reduced = matrix.span_contains(vector);

//         auto end = std::chrono::high_resolution_clock::now();
//         std::chrono::duration<double, std::nano> duration = end - start;

//         total_time += duration.count();
//     }
//     std::cout << "Total time (reduce): " << total_time / 1000 / N << " µs per call\n";
// }

// void test_ref_iter(int N) {
//     auto total_time = 0.;

//     for (int i = 0; i < N; ++i) {
//         auto [matrix, vector] = generate_data();
//         auto start = std::chrono::high_resolution_clock::now();
        
//         matrix.push_to_echelon_form(vector);
        
//         auto end = std::chrono::high_resolution_clock::now();
//         std::chrono::duration<double, std::nano> duration = end - start;

//         total_time += duration.count();
//     }
//     std::cout << "Total time (custom): " << total_time / 1000 / N << " µs per call\n";
// }

// void test_ref(int N) {
//     auto total_time = 0.;

//     for (int i = 0; i < N; ++i) {
//         auto [matrix, vector] = generate_data();
//         auto start = std::chrono::high_resolution_clock::now();
        
//         matrix.push_row(vector);
//         matrix.to_echelon_form();

//         auto end = std::chrono::high_resolution_clock::now();
//         std::chrono::duration<double, std::nano> duration = end - start;

//         total_time += duration.count();
//     }
//     std::cout << "Total time (original): " << total_time / 1000 / N << " µs per call\n";
// }

// void test_pivot(int N) {
//     auto total_time = 0.;
//     auto pivot_totals = std::vector<std::size_t>(100);
//     auto first_totals = std::vector<std::size_t>(100);
//     for (int i = 0; i < N; ++i) {
//         auto [matrix, vector] = generate_data();
//         std::size_t first = matrix.reduce(vector).first_set();
//         first_totals[first] += 1;
//         for (int j = 0; j < 100; ++j) {
//             pivot_totals[j] += matrix.get_pivot_cols()->element(j);
//         }

//     }
//     std::cout << "Pivot distribution ";
//     for (int i = 0; i < 100; ++i) {
//         std::cout << pivot_totals[i] << " ";
//     }
//     std::cout << std::endl;
//     std::cout << "First distribution ";
//     for (int i = 0; i < 100; ++i) {
//         std::cout << first_totals[i] << " ";
//     }
//     std::cout << std::endl;
// }

std::tuple<std::size_t, double, double> run_10_seconds(Solver& solver) {
    std::size_t n_times = 0;
    double total_clauses = 0;
    double total_time = 0.;

    while (total_time < 10000000000) {
        auto solver_copy = solver;
        solver.reinitialize_decision_procedure();
        auto start = std::chrono::high_resolution_clock::now();
        auto solution = solver_copy.solve();
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::nano> duration = end - start;
        auto time = duration.count();

        n_times += 1;
        total_clauses += solver_copy.get_num_clauses();
        total_time += time;
    }
    
    return std::make_tuple(n_times, total_time / n_times, total_clauses / n_times);
}

int main(int argc, char *argv[]) {
    std::string filename;
    bool help{false};
    bool verbose{false};

    static constexpr struct option long_options[] = {
        {"file", required_argument, 0, 'f'},
        {"help", no_argument, 0, 'h'},
        {"verbose", no_argument, 0, 'v'},
        {0, 0, 0, 0}
    };

    int option_index{0};
    while (true) {
        const int option = getopt_long(argc, argv, "f:hv", long_options, &option_index);
        if (option == -1) {
            break;
        }
        switch (option) {
            case 'f':
                filename = optarg;
                break;
            case 'h':
                help = true;
                break;
            case 'v':
                verbose = true;
                break;
            default:
                std::cerr << "Usage: " << argv[0] << " [options] <filename>" << std::endl;
                return 1;
        }
    }

    if (help) {
        std::cout << "Usage: " << argv[0] << " [options] <filename>" << std::endl;
        std::cout << "Options:" << std::endl;
        std::cout << "  -f, --file <filename>  Specify the filename" << std::endl;
        std::cout << "  -h, --help             Show this help message" << std::endl;
        std::cout << "  -v, --verbose          Verbose mode" << std::endl;
        return 0;
    }

    if (filename.empty()) {
        std::cerr << "Filename is required. Usage: " << argv[0] << " -f <filename> [options]" << std::endl;
        return 1;
    }

    if (verbose) {
        std::cout << "Verbose mode enabled" << std::endl;
    }

    Solver solver = Solver::from_DIMACS(filename);
    std::cout << "Solver with " << solver.get_num_vars() << " variables, " << solver.get_num_clauses() << " clauses:" << std::endl;

    auto [n_times, time, avg_clauses] = run_10_seconds(solver);
    std::cout << "Solved " << n_times << " times, "
                << time / 1000000 << " ms per call, "
                << avg_clauses << " clauses per call" << std::endl;

    // int N = 10000;

    // test_contains(N);
    // test_ref_iter(N);
    // test_ref(N);
    // test_pivot(N);

    // test();

    return 0;
}
