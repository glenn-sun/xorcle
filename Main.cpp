#include <iostream>
#include <getopt.h>
#include "Solver.h"


void test_rref_iter() {
    // Create a random 5x10 bit matrix
    bit::matrix<uint64_t> matrix = bit::matrix<uint64_t>::random(5, 10);

    // Compute row echelon form
    bit::vector<uint64_t> pivot_cols(10);
    matrix.to_reduced_echelon_form(&pivot_cols);

    // Print the matrix in row echelon form
    std::cout << "Row Echelon Form:" << std::endl;
    std::cout << matrix.to_pretty_string() << std::endl;

    // Create a random bit vector with 10 entries
    bit::vector<uint64_t> vector = bit::vector<uint64_t>::random(10);
    std:: cout << "Vector:" << std::endl << vector.to_pretty_string() <<    std::endl;

    std::cout << "Span contains:" << matrix.span_contains(vector, pivot_cols) << std::endl;
 
    // Try to reduce the vector using the matrix in row echelon form
    matrix.push_to_reduced_echelon_form(vector, pivot_cols);

    // Print the final result
    std::cout << "Final Result:" << std::endl;
    std::cout << matrix.to_pretty_string() << std::endl;

    auto reduced = matrix.reduce_copy(vector, pivot_cols);
    std:: cout << "Reduced:" << std::endl << reduced.to_pretty_string() <<    std::endl;

    std::cout << "Span contains:" << matrix.span_contains(vector, pivot_cols) << std::endl;
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

    // Solver solver = from_DIMACS(filename);
    // solver.print();

    test_rref_iter();

    return 0;
}
