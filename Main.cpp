#include <iostream>
#include <getopt.h>
#include "Solver.h"

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

    Solver solver = from_DIMACS(filename);
    solver.print();

    return 0;
}

